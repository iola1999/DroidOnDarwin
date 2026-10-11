#!/usr/bin/env python3
"""为 PTE 翻译构建接入双 CPU 的执行登记与停止协议。"""
from pathlib import Path


def apply(source: Path, experiment: Path):
    def edit(path, before, after):
        text = path.read_text()
        if after in text:
            return
        if text.count(before) != 1:
            raise RuntimeError(f'Translation SMP source mismatch: {path}: {before[:90]}')
        path.write_text(text.replace(before, after))

    arch = source / 'arch/lkl'
    host = arch / 'include/uapi/asm/host_ops.h'
    edit(host, '\tvoid (*user_interrupt)(unsigned int mask);',
         '\tvoid (*user_interrupt)(unsigned int mask);\n\tvoid (*user_prepare)(unsigned int cpu);')
    mmu = arch / 'mm/nce_mmu.c'
    text = mmu.read_text()
    # Allocate/free and invalidation fanout share the writer lock. Guest code
    # never enters a Linux lock while retaining its execution registration.
    signature = 'int nce_init_mm(struct task_struct *task, struct mm_struct *mm)'
    start = text.index(signature + '\n{')
    end = text.index('\n}', start) + 2
    body = text[start:end].replace('int nce_init_mm(', 'static int init_translation_mm(', 1)
    wrapper = signature + '''
{
    nce_window_lock();
    int result = init_translation_mm(task, mm);
    nce_window_unlock();
    return result;
}'''
    text = text[:start] + body + '\n\n' + wrapper + text[end:]
    for signature in ('void nce_destroy_mm(struct mm_struct *mm)',
                      'void nce_flush_range(struct mm_struct *mm, unsigned long start, unsigned long end)'):
        start = text.index(signature + '\n{')
        end = text.index('\n}', start) + 2
        body = text[start:end].replace('    nce_window_lock();', '    nce_window_lock();\n    nce_window_stop();')
        text = text[:start] + body + text[end:]
    start = text.index('long nce_publish_code(')
    end = text.index('\n}', start) + 2
    text = text[:start] + text[start:end].replace('    nce_window_lock();', '    nce_window_lock();\n    nce_window_stop();') + text[end:]
    start = text.index('void nce_begin_user(void)')
    end = text.index('\n}', start) + 2
    text = text[:start] + '''unsigned long nce_translation_enter(void)
{
    for (;;) {
        unsigned long flags;
        local_irq_save(flags);
        u64 token = nce_window_token();
        lkl_ops->user_prepare(raw_smp_processor_id());
        if (nce_window_try_enter(token)) {
            nce_prepare_user();
            return flags;
        }
        local_irq_restore(flags);
        cond_resched();
    }
}

void nce_translation_leave(unsigned long flags)
{
    nce_window_leave();
    local_irq_restore(flags);
}

void nce_translation_exclusive(void)
{
    nce_window_lock();
    nce_window_stop();
    nce_prepare_user();
}''' + text[end:]
    marker = '''    if (translation->code_guard) {
        for (unsigned long i = index; i < index + pages; ++i)
            translation->code_guard[i] = 1;
    }
'''
    if text.count(marker) != 1:
        raise RuntimeError('Translation code guard marker mismatch')
    text = text.replace(marker, '')
    text += '''

/* Count executable PTEs, including fork aliases. Retired mappings must not
 * keep recycled data pages on the serialized code-write path. */
static unsigned int executable_ptes[NCE_RAM_SIZE / PAGE_SIZE];

void nce_translation_pte_changed(struct mm_struct *mm, pte_t old, pte_t next)
{
    if (mm == &init_mm)
        return;
    unsigned long before = pte_present(old) && pte_exec(old) ? pte_val(old) & PAGE_MASK : 0;
    unsigned long after = pte_present(next) && pte_exec(next) ? pte_val(next) & PAGE_MASK : 0;
    if (before == after)
        return;
    if (before) {
        unsigned long index = (before - memory_start) >> PAGE_SHIFT;
        BUG_ON(index >= ARRAY_SIZE(executable_ptes) || !executable_ptes[index]);
        translation->code_guard[index] = --executable_ptes[index] != 0;
    }
    if (after) {
        unsigned long index = (after - memory_start) >> PAGE_SHIFT;
        BUG_ON(index >= ARRAY_SIZE(executable_ptes) || executable_ptes[index] == UINT_MAX);
        ++executable_ptes[index];
        translation->code_guard[index] = 1;
    }
}

bool nce_translation_access_begin(unsigned long address, bool write)
{
    if (translation->uaccess_code_only && !write)
        return false;
    nce_window_lock();
    unsigned long index = (address - memory_start) >> PAGE_SHIFT;
    BUG_ON(index >= NCE_RAM_SIZE / PAGE_SIZE);
    if (!translation->uaccess_code_only || translation->code_guard[index])
        nce_window_stop();
    return true;
}

void nce_translation_written(unsigned long address, unsigned long length)
{
    if (!length || address < memory_start || address - memory_start >= NCE_RAM_SIZE)
        return;
    unsigned long offset = address - memory_start;
    BUG_ON(length > NCE_RAM_SIZE - offset);
    unsigned long end = PAGE_ALIGN(offset + length);
    for (unsigned long at = offset & PAGE_MASK; at < end; at += PAGE_SIZE) {
        if (translation->code_guard[at >> PAGE_SHIFT])
            changed_code(__pte(memory_start + at), 1);
    }
}
'''
    mmu.write_text(text)
    table = arch / 'include/asm/pgtable-mmu-3level.h'
    text = table.read_text().replace('\tnce_window_lock();', '\tnce_window_lock();\n\tnce_window_stop();')
    declaration = 'void munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp);'
    text = text.replace(declaration, declaration + '\nvoid nce_translation_pte_changed(struct mm_struct *, pte_t, pte_t);')
    text = text.replace('\tpte_set_val(*(xp), 0, __pgprot(_PAGE_NEWPAGE));',
                        '\tnce_translation_pte_changed(mm, *xp, __pte(0));\n\tpte_set_val(*(xp), 0, __pgprot(_PAGE_NEWPAGE));')
    text = text.replace('\t\t__set_pte(ptep, pte);',
                        '\t\tnce_translation_pte_changed(mm, *ptep, pte);\n\t\t__set_pte(ptep, pte);')
    table.write_text(text)

    access = arch / 'mm/nce_smp_uaccess.c'
    edit(access, '#include <asm/nce-smp-uaccess.h>',
         '#include <asm/nce-smp-uaccess.h>\n#include <asm/nce-window.h>\n'
         'extern bool nce_translation_access_begin(unsigned long address, bool write);\n'
         'extern void nce_translation_written(unsigned long address, unsigned long length);')
    edit(access, '    void *alias;', '    void *alias;\n    bool write, writer;')
    edit(access, '    access->alias = page_address(pte_page(value)) + offset_in_page(address);',
         '    access->alias = page_address(pte_page(value)) + offset_in_page(address);\n'
         '    access->write = write;\n'
         '    access->writer = nce_translation_access_begin((unsigned long)access->alias, write);')
    edit(access, 'static void end_access(struct user_access *access)\n{',
         'static void end_access(struct user_access *access)\n{\n'
         '    if (access->write)\n'
         '        nce_translation_written((unsigned long)access->alias & PAGE_MASK, PAGE_SIZE);\n'
         '    if (access->writer)\n        nce_window_unlock();')

    setup = arch / 'kernel/setup.c'
    edit(setup, '#include <linux/init.h>', '#include <linux/init.h>\n#include <linux/rcupdate.h>')
    edit(setup, 'void machine_halt(void)', 'extern void lkl_flush_netns(void);\n\nvoid machine_halt(void)')
    edit(setup, '\tlkl_cpu_shutdown();',
         '\t/* Deferred task and mm destruction needs both CPUs still online. */\n'
         '\trcu_barrier();\n\tlkl_flush_netns();\n\trcu_barrier();\n\tlkl_cpu_shutdown();')

    namespaces = source / 'net/core/net_namespace.c'
    edit(namespaces, 'static DECLARE_WORK(net_cleanup_work, cleanup_net);',
         'static DECLARE_WORK(net_cleanup_work, cleanup_net);\n\n'
         '/* The LKL host has stopped userspace. Namespace cleanup can itself\n'
         ' * wait for RCU callbacks, so finish it before stopping either CPU. */\n'
         'void lkl_flush_netns(void)\n{\n\tflush_workqueue(netns_wq);\n}')
