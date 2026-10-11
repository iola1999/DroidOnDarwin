#!/usr/bin/env python3
"""为独立的双 CPU 内核原型修改 LKL；输入先应用当前 NCE 构建补丁。"""
from pathlib import Path


def apply(source: Path, experiment: Path, *, tests=True):
    def edit(name, before, after):
        path = source / name
        text = path.read_text()
        if after in text:
            return
        if text.count(before) != 1:
            raise RuntimeError(f'SMP source mismatch: {name}: {before[:80]}')
        path.write_text(text.replace(before, after))

    def copy(name, destination):
        content = (experiment / 'smp' / name).read_bytes()
        if not destination.exists() or destination.read_bytes() != content:
            destination.write_bytes(content)

    arch = 'arch/lkl/'
    edit(arch + 'Kconfig', 'depends on !SMP && !COREDUMP', 'depends on !COREDUMP')
    if 'config SMP\n' not in (source / arch / 'Kconfig').read_text():
        edit(arch + 'Kconfig', 'config LKL_FUZZING', '''config SMP
	bool "Experimental LKL SMP"
	depends on !MMU
	select ARCH_USE_QUEUED_RWLOCKS
	select GENERIC_SMP_IDLE_THREAD
	default y

config NR_CPUS
	int
	default 2

config LKL_FUZZING''')
    edit(arch + 'include/asm/Kbuild', 'generic-y += cmpxchg.h', '# SMP cmpxchg uses AArch64 atomic instructions')
    for name in ['cmpxchg.h', 'barrier.h', 'smp.h', 'futex.h']:
        copy(name, source / arch / 'include/asm' / name)
    for name, include in [('spinlock.h', 'spinlock.h'), ('spinlock_types.h', 'spinlock_types.h'),
                          ('qrwlock.h', 'qrwlock.h'), ('qrwlock_types.h', 'qrwlock_types.h')]:
        destination = source / arch / 'include/asm' / name
        content = f'#include <asm-generic/{include}>\n'
        if not destination.exists() or destination.read_text() != content:
            destination.write_text(content)
    for name in ['smp.c', 'time.c', *(['test.c'] if tests else [])]:
        copy(name, source / arch / 'kernel' / ('smp-test.c' if name == 'test.c' else name))
    kernel_objects = 'smp.o smp-test.o' if tests else 'smp.o'
    if 'obj-$(CONFIG_SMP) += ' + kernel_objects not in (source / arch / 'kernel/Makefile').read_text():
        edit(arch + 'kernel/Makefile', 'obj-y = setup.o',
             'obj-$(CONFIG_SMP) += ' + kernel_objects + '\nobj-y += setup.o')
    edit('kernel/sched/core.c', 'int __sched __cond_resched(void)\n{',
         'int __sched __cond_resched(void)\n{\n#ifdef CONFIG_LKL\n\tcpu_relax();\n#endif')
    # Early SMP setup can panic before the printk console is registered.
    edit('kernel/panic.c', '#include <linux/console.h>',
         '#include <linux/console.h>\n#include <asm/host_ops.h>')
    edit('kernel/panic.c', '\tlen = vscnprintf(buf, sizeof(buf), fmt, args);',
         '\tlen = vscnprintf(buf, sizeof(buf), fmt, args);\n\tlkl_ops->print(buf, len);')
    edit('include/asm-generic/bug.h', 'panic("BUG!");',
         'panic("BUG at %s:%d/%s()", __FILE__, __LINE__, __func__);')

    edit(arch + 'include/asm/thread_info.h', '#include <asm/host_ops.h>',
         '#include <asm/host_ops.h>\n#include <asm/smp.h>\n#include <linux/threads.h>')
    edit(arch + 'include/asm/thread_info.h', '\tint preempt_count;', '\tint preempt_count;\n\tunsigned int cpu;')
    edit(arch + 'include/asm/processor.h', '#define task_pt_regs(tsk) (struct pt_regs *)(NULL)',
         '#define task_pt_regs(tsk) ((struct pt_regs *)(NULL))')
    edit(arch + 'include/asm/thread_info.h',
         'extern struct thread_info *_current_thread_info;',
         'extern struct thread_info *lkl_current_threads[NR_CPUS];')
    edit(arch + 'include/asm/thread_info.h', 'return _current_thread_info;',
         'return lkl_current_threads[raw_smp_processor_id()];')
    edit(arch + 'kernel/threads.c',
         'struct thread_info *_current_thread_info = &init_thread_union.thread_info;',
         'struct thread_info *lkl_current_threads[NR_CPUS] = {&init_thread_union.thread_info};\n'
         '#define _current_thread_info lkl_current_threads[raw_smp_processor_id()]\n'
         'void lkl_smp_set_current(unsigned int cpu, struct task_struct *task)\n'
         '{\n\tlkl_current_threads[cpu] = task_thread_info(task);\n}')
    edit(arch + 'kernel/threads.c', 'static struct task_struct *abs_prev = &init_task;',
         'static struct task_struct *previous_tasks[NR_CPUS] = {&init_task};\n'
         '#define abs_prev previous_tasks[raw_smp_processor_id()]')
    edit(arch + 'kernel/threads.c', '\n\tif (_prev->dead) {',
         '\n\tlkl_set_current_cpu(task_cpu(prev));\n\tif (_prev->dead) {')
    edit(arch + 'kernel/threads.c', '\tlkl_ops->sem_down(ti->sched_sem);\n\tkfree(tba);',
         '\tlkl_ops->sem_down(ti->sched_sem);\n\tlkl_set_current_cpu(task_cpu(ti->task));\n\tkfree(tba);')
    edit(arch + 'kernel/threads.c', '\ttba->f = (int (*)(void *))esp;',
         '\ttba->f = args->idle ? lkl_secondary_start : (int (*)(void *))esp;')

    edit(arch + 'kernel/cpu.c', '#include <linux/cpu.h>', '#include <linux/cpu.h>\n#include <linux/tick.h>')
    edit(arch + 'kernel/cpu.c', '} cpu;',
         '} cpus[NR_CPUS];\n#define cpu cpus[raw_smp_processor_id()]')
    # A previously woken idle host task can be the last runnable task after
    # shutdown. It must schedule the Linux idle task, which acknowledges stop;
    # releasing CPU ownership would leave it asleep with IRQ delivery disabled.
    edit(arch + 'kernel/cpu.c',
         '!single_task_running() && cpu.count == 1 && !(idle_handoff && cpu.sleepers))',
         'cpu.count == 1 &&\n'
         '\t    (READ_ONCE(cpu.shutdown_gate) >= MAX_THREADS ||\n'
         '\t     (!single_task_running() && !(idle_handoff && cpu.sleepers))))')
    edit(arch + 'kernel/cpu.c', '\t__sync_fetch_and_add(&cpu.shutdown_gate, MAX_THREADS);',
         '\tunsigned int index;\n\tfor_each_online_cpu(index) {\n'
         '\t\t__sync_fetch_and_add(&cpus[index].shutdown_gate, MAX_THREADS);\n'
         '\t\tlkl_smp_wake(index);\n\t}')
    edit(arch + 'kernel/cpu.c',
         '\tlkl_ops->sem_down(cpu.shutdown_sem);\n\tlkl_ops->sem_free(cpu.shutdown_sem);',
         '\tunsigned int index;\n\tfor_each_online_cpu(index) {\n'
         '\t\tlkl_ops->sem_down(cpus[index].shutdown_sem);\n'
         '\t\tlkl_ops->sem_free(cpus[index].shutdown_sem);\n\t}\n\tlkl_smp_join();')
    edit(arch + 'kernel/cpu.c', '\t\tlkl_cpu_cleanup(true);',
         '\t\ttick_suspend_local();\n\t\tlkl_cpu_cleanup(true);')
    edit(arch + 'kernel/cpu.c', '\t/* switch to idle_host_task */',
         '\tif (raw_smp_processor_id()) {\n\t\tlkl_smp_idle();\n\t\treturn;\n\t}\n'
         '\t/* switch to idle_host_task */')
    edit(arch + 'kernel/cpu.c',
         '\tcpu.lock = lkl_ops->mutex_alloc(0);\n\tcpu.sem = lkl_ops->sem_alloc(0);\n'
         '\tcpu.shutdown_sem = lkl_ops->sem_alloc(0);\n\n'
         '\tif (!cpu.lock || !cpu.sem || !cpu.shutdown_sem) {\n'
         '\t\tlkl_cpu_cleanup(false);\n\t\treturn -ENOMEM;\n\t}',
         '\tunsigned int index;\n\tlkl_smp_init();\n\tfor (index = 0; index < NR_CPUS; ++index) {\n'
         '\t\tcpus[index].lock = lkl_ops->mutex_alloc(0);\n'
         '\t\tcpus[index].sem = lkl_ops->sem_alloc(0);\n'
         '\t\tcpus[index].shutdown_sem = lkl_ops->sem_alloc(0);\n'
         '\t\tif (!cpus[index].lock || !cpus[index].sem || !cpus[index].shutdown_sem)\n'
         '\t\t\tlkl_bug("cannot allocate LKL CPU ownership state\\n");\n\t}')

    edit(arch + 'kernel/irq.c', 'static unsigned long irq_status[NR_IRQS/IRQ_STATUS_BITS];',
         'static unsigned long cpu_irq_status[NR_CPUS][NR_IRQS/IRQ_STATUS_BITS];\n'
         '#define irq_status cpu_irq_status[raw_smp_processor_id()]')
    edit(arch + 'kernel/irq.c', 'static unsigned long irq_index_status;',
         'static unsigned long cpu_irq_index[NR_CPUS];\n'
         '#define irq_index_status cpu_irq_index[raw_smp_processor_id()]')
    edit(arch + 'kernel/irq.c', 'static bool irqs_enabled;',
         'static bool cpu_irqs_enabled[NR_CPUS];\n'
         '#define irqs_enabled cpu_irqs_enabled[raw_smp_processor_id()]')
    edit(arch + 'kernel/irq.c', 'static struct pt_regs dummy;',
         'static struct pt_regs irq_regs[NR_CPUS];\n#define dummy irq_regs[raw_smp_processor_id()]')

    edit(arch + 'kernel/syscalls.c', 'int syscalls_init(void)\n{',
         'int syscalls_init(void)\n{\n\tif (set_cpus_allowed_ptr(current, cpumask_of(0)))\n'
         '\t\treturn -EINVAL;')
    if tests:
        edit(arch + 'kernel/syscalls.c', '\thandler = (syscall_handler_t)syscall_table[no];',
             '\tif (no == 244)\n\t\treturn lkl_smp_test();\n'
             '\thandler = (syscall_handler_t)syscall_table[no];')
    edit(arch + 'kernel/setup.c', '\tbootmem_init(mem_size);',
         '\tlkl_smp_prepare_possible();\n\tbootmem_init(mem_size);')
    edit(arch + 'kernel/setup.c', '\ttick_suspend_local();',
         '\ttick_suspend_local();\n\tlkl_smp_cleanup();')
