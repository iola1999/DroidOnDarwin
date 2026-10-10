/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "linux-execution.h"
#include "guest-registers.h"
#include "layout.h"
#include "lkl-host-ops.h"
#include "lkl-image.h"
#include <elf.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unicorn/unicorn.h>

#define REQUIRE(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "translation:%d: %s\n", __LINE__, #c); \
            abort(); \
        } \
    } while (0)
#define PAGE NCE_PAGE_SIZE
#define RESERVATION (NCE_USER_SIZE + NCE_RAM_SIZE + NCE_VMALLOC_SIZE)

typedef struct {
    struct pt_regs regs;
    unsigned long tls;
    const char *path;
} Process;
static unsigned char *ram;
static unsigned long base, *pages;
static unsigned long map_generation, code_generation, maps, revocations, fills, faults, calls, preemptions;
static unsigned long allocated, freed;
static unsigned long cache_flushes, cache_pages, tlb_flushes;
static bool page_invalidation;
static unsigned long instruction_batch;
static bool timer_mode, timer_active, timer_stopping;
static pthread_t timer_thread;
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_condition = PTHREAD_COND_INITIALIZER;
static uc_engine *engine;
static uc_context *empty_cpu;
static long (*run_syscall)(long, long *);
static int (*page_fault)(unsigned long, int);
static void (*user_return)(long, long, unsigned long), (*user_fault)(unsigned long, int);
static void (*user_preempt)(void), (*prepare_user)(void);
static unsigned long (*copy_user)(void *, unsigned long, unsigned long, int);
static unsigned long (*translate_user)(unsigned long, unsigned long);
static struct nce_translation_state *translation;
extern struct lkl_host_operations nce_lkl_host;
extern uc_err nce_uc_emu_stop_at_tb(uc_engine *engine);
extern uc_err nce_uc_invalidate_ram(uc_engine *engine, uint64_t start, uint64_t length);
extern uc_err nce_uc_set_linux_memory(uc_engine *engine, bool enabled);

static void checked(uc_err error)
{
    if (error != UC_ERR_OK) {
        fprintf(stderr, "Unicorn: %s\n", uc_strerror(error));
        abort();
    }
}

static void *preempt_timer(void *unused)
{
    (void)unused;
    REQUIRE(pthread_mutex_lock(&timer_lock) == 0);
    while (!timer_stopping) {
        struct timespec interval = {.tv_nsec = 2000000};
        int error = pthread_cond_timedwait_relative_np(&timer_condition, &timer_lock, &interval);
        REQUIRE(error == 0 || error == ETIMEDOUT);
        /* Async stopping must finish the current guest instruction. The lock
         * excludes register/cache operations and engine destruction. */
        if (timer_active && !timer_stopping)
            checked(nce_uc_emu_stop_at_tb(engine));
    }
    REQUIRE(pthread_mutex_unlock(&timer_lock) == 0);
    return NULL;
}

static void mark_executing(bool active)
{
    if (!timer_mode)
        return;
    REQUIRE(pthread_mutex_lock(&timer_lock) == 0);
    timer_active = active;
    REQUIRE(pthread_mutex_unlock(&timer_lock) == 0);
}

static void remap(void *target, void *source, unsigned long size, int prot)
{
    vm_address_t address = (uintptr_t)target;
    vm_prot_t current, maximum;
    REQUIRE(vm_remap(mach_task_self(), &address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                     mach_task_self(), (uintptr_t)source, FALSE, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    REQUIRE(mprotect(target, size, prot) == 0);
}

static void shmem_init(unsigned long size)
{
    REQUIRE(size == NCE_RAM_SIZE);
    ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(ram != MAP_FAILED);
    checked(uc_mem_map_ptr(engine, 0, size, UC_PROT_ALL, ram));
}

static int shmem_reclaim(unsigned long offset, unsigned long size)
{
    REQUIRE(!(offset % PAGE) && !(size % PAGE) && offset <= NCE_RAM_SIZE && size <= NCE_RAM_SIZE - offset);
    REQUIRE(mmap(ram + offset, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == ram + offset);
    remap((void *)(base + NCE_USER_SIZE + offset), ram + offset, size, PROT_READ | PROT_WRITE);
    if (translation) {
        for (unsigned long i = offset / PAGE; i < (offset + size) / PAGE; ++i)
            translation->code_pages[i / 64] |= 1UL << (i % 64);
        ++translation->code;
    } else
        ++code_generation;
    return 0;
}

static void *shmem_map(void *address, unsigned long offset, unsigned long size, enum lkl_prot prot)
{
    unsigned long start = (uintptr_t)address;
    REQUIRE(start >= base && !(start % PAGE) && !(offset % PAGE) && !(size % PAGE));
    REQUIRE(offset <= NCE_RAM_SIZE && size <= NCE_RAM_SIZE - offset);
    if (start >= base + NCE_USER_SIZE) {
        REQUIRE(start <= base + RESERVATION && size <= base + RESERVATION - start);
        remap(address, ram + offset, size, prot);
        return address;
    }
    REQUIRE(!translation);
    REQUIRE(size <= base + NCE_USER_SIZE - start);
    for (unsigned long i = 0; i < size; i += PAGE) {
        unsigned long index = (start + i - base) / PAGE;
        unsigned long value = (offset + i) | 8 | prot;
        if (pages[index] != value) {
            if ((pages[index] | value) & LKL_PROT_EXEC)
                ++code_generation;
            pages[index] = value;
            ++map_generation;
        }
    }
    ++maps;
    return address;
}

static int revoke_pages(void *address, unsigned long size)
{
    unsigned long start = (uintptr_t)address;
    REQUIRE(start >= base && !(start % PAGE) && !(size % PAGE));
    REQUIRE(start <= base + RESERVATION && size <= base + RESERVATION - start);
    if (start < base + NCE_USER_SIZE) {
        REQUIRE(!translation);
        unsigned long length = size < base + NCE_USER_SIZE - start ? size : base + NCE_USER_SIZE - start;
        for (unsigned long i = 0; i < length; i += PAGE) {
            unsigned long index = (start + i - base) / PAGE;
            if (pages[index]) {
                if (pages[index] & LKL_PROT_EXEC)
                    ++code_generation;
                pages[index] = 0;
                ++map_generation;
            }
        }
        start += length;
        size -= length;
        ++revocations;
    }
    if (size)
        REQUIRE(mmap((void *)start, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (void *)start);
    return 0;
}

static struct {
    int kind, access;
    uint64_t address;
} event;

static void fault_event(uint64_t address, uc_mem_type access)
{
    event.kind = 2;
    event.address = address;
    event.access = access == UC_MEM_WRITE || access == UC_MEM_WRITE_PROT || access == UC_MEM_WRITE_UNMAPPED ? 1 : access == UC_MEM_FETCH || access == UC_MEM_FETCH_PROT || access == UC_MEM_FETCH_UNMAPPED ? NCE_FAULT_EXEC
                                                                                                                                                                                                           : 0;
}

static bool fill(uc_engine *uc, uint64_t address, uc_mem_type access, uc_tlb_entry *entry, void *opaque)
{
    (void)uc;
    (void)opaque;
    ++fills;
    unsigned permission = access == UC_MEM_WRITE ? UC_PROT_WRITE : access == UC_MEM_FETCH ? UC_PROT_EXEC
                                                                                          : UC_PROT_READ;
    if (address >= base && address < base + NCE_USER_SIZE) {
        unsigned long value = translate_user ? translate_user(address, permission) : pages[(address - base) / PAGE];
        if (value & permission) {
            REQUIRE((value & ~(PAGE - 1)) < NCE_RAM_SIZE);
            entry->paddr = value & ~(PAGE - 1);
            entry->perms = value & UC_PROT_ALL;
            return true;
        }
    }
    fault_event(address, access);
    return false;
}

static void interrupt(uc_engine *uc, uint32_t number, void *opaque)
{
    (void)opaque;
    event.kind = number == 2 ? 1 : 3;
    checked(uc_emu_stop(uc));
}

static void registers(Process *p, bool save)
{
    int ids[69], count = 0;
    void *values[69];
    for (int i = 0; i < 31; ++i) {
        ids[count] = i < 29 ? UC_ARM64_REG_X0 + i : i == 29 ? UC_ARM64_REG_X29
                                                            : UC_ARM64_REG_X30;
        values[count++] = &p->regs.x[i];
    }
    const int extra[] = {UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_NZCV, UC_ARM64_REG_FPCR, UC_ARM64_REG_FPSR};
    unsigned long *slots[] = {&p->regs.sp, &p->regs.pc, &p->regs.nzcv, &p->regs.fpcr, &p->regs.fpsr};
    for (int i = 0; i < 5; ++i) {
        ids[count] = extra[i];
        values[count++] = slots[i];
    }
    for (int i = 0; i < 32; ++i) {
        ids[count] = UC_ARM64_REG_Q0 + i;
        values[count++] = p->regs.vector[i];
    }
    ids[count] = UC_ARM64_REG_TPIDR_EL0;
    values[count++] = &p->tls;
    REQUIRE(count == 69);
    checked(save ? uc_reg_read_batch(engine, ids, values, count) : uc_reg_write_batch(engine, ids, values, count));
}

static void *user_clone(void *parent, unsigned long sp, unsigned long tls, unsigned long flags)
{
    Process *p = calloc(1, sizeof(*p));
    REQUIRE(p);
    *p = *(Process *)parent;
    p->path = NULL;
    p->regs.x[0] = 0;
    if (sp)
        p->regs.sp = sp;
    if (flags & 0x80000)
        p->tls = tls;
    ++allocated;
    return p;
}

static void user_free(void *p)
{
    ++freed;
    free(p);
}

static void user_switch(void *p, unsigned cpu, unsigned cpus)
{
    (void)p;
    REQUIRE(cpu == 0 && cpus == 1);
}

void nce_process_thread_exit(void) {}

static void user_exec(void *opaque, unsigned long entry, unsigned long stack)
{
    Process *p = opaque;
    memset(&p->regs, 0, sizeof(p->regs));
    p->regs.pc = entry;
    p->regs.sp = stack;
    p->tls = 0;
    ++code_generation;
}

static void user_run(void *opaque)
{
    Process *p = opaque;
    if (!p->regs.sp) {
        long args[6] = {base + NCE_USER_SIZE - 65536, 65536, 3, 0x32, -1, 0};
        REQUIRE(run_syscall(222, args) == args[0]);
        p->regs.sp = args[0] + args[1];
    }
    if (p->path) {
        unsigned long storage[64] = {0}, address = p->regs.sp - sizeof(storage);
        REQUIRE(strlen(p->path) < 64);
        strcpy((char *)storage + 128, p->path);
        strcpy((char *)storage + 192, "NCE_TEST=translation");
        storage[0] = address + 128;
        storage[2] = address + 192;
        REQUIRE(copy_user(storage, address, sizeof(storage), 1) == 0);
        long args[6] = {address + 128, address, address + 16};
        long result = run_syscall(221, args);
        if (result)
            fprintf(stderr, "exec %s: %ld\n", p->path, result);
        REQUIRE(result == 0);
        p->path = NULL;
    }
    for (;;) {
        prepare_user();
        /* Each entry starts with no exclusive reservation. Linux may have
         * switched threads or installed a signal frame during the previous exit. */
        checked(uc_context_restore(engine, empty_cpu));
        static unsigned long seen_maps, seen_code, seen_kernel_code;
        unsigned long current_maps = translation ? translation->mappings : map_generation;
        unsigned long current_kernel_code = translation ? translation->code : 0;
        if (seen_code != code_generation || seen_kernel_code != current_kernel_code) {
            bool all = seen_code != code_generation || !page_invalidation || !translation;
            if (all) {
                checked(uc_ctl_flush_tb(engine));
                ++cache_flushes;
            }
            if (translation) {
                for (unsigned i = 0; i < NCE_TRANSLATION_CODE_WORDS; ++i) {
                    unsigned long bits = translation->code_pages[i];
                    while (bits && !all) {
                        unsigned bit = __builtin_ctzl(bits);
                        checked(nce_uc_invalidate_ram(engine, (i * 64UL + bit) * PAGE, PAGE));
                        ++cache_pages;
                        bits &= bits - 1;
                    }
                    translation->code_pages[i] = 0;
                }
            }
            seen_code = code_generation;
            seen_kernel_code = current_kernel_code;
        }
        if (seen_maps != current_maps) {
            checked(uc_ctl_flush_tlb(engine));
            ++tlb_flushes;
            seen_maps = current_maps;
        }
        registers(p, false);
        memset(&event, 0, sizeof(event));
        mark_executing(true);
        uc_err error = uc_emu_start(engine, p->regs.pc, 0, 0, timer_mode ? 0 : instruction_batch);
        mark_executing(false);
        registers(p, true);
        /* Copy callback state before entering Linux: a blocking syscall or
         * page fault can let another Linux thread use this same engine. */
        int kind = event.kind, access = event.access;
        unsigned long address = event.address;
        if (kind == 1) {
            long args[6];
            memcpy(args, p->regs.x, sizeof(args));
            long number = p->regs.x[8];
            ++calls;
            long result = run_syscall(number, args);
            if (number == NCE_SYS_PUBLISH_CODE && !translation)
                ++code_generation;
            user_return(number, result, args[0]);
        } else if (kind == 2) {
            ++faults;
            int result = page_fault(address, access);
            if (result) {
                fprintf(stderr, "guest fault pc=%lx address=%lx access=%d result=%d\n", p->regs.pc, address, access, result);
                user_fault(address, 0);
            }
        } else if (kind == 3 || error != UC_ERR_OK) {
            fprintf(stderr, "guest exception pc=%lx error=%s event=%d\n", p->regs.pc, uc_strerror(error), kind);
            uint32_t instruction = 0;
            bool breakpoint = !copy_user(&instruction, p->regs.pc, sizeof(instruction), 0) &&
                              (instruction & 0xffe0001f) == 0xd4200000;
            user_fault(p->regs.pc, breakpoint ? 2 : 1);
        } else {
            ++preemptions;
            user_preempt();
        }
    }
}

/* Runtime ELF placement is intentionally separate from the native program
 * registry. This bounded probe retains each file's original code and headers. */
static struct {
    char *path;
    unsigned long offset, base, span;
} layouts[64];
static unsigned layout_count;
static unsigned long cursor;

static unsigned long elf_layout(const char *path, unsigned long offset, const void *raw_header, const void *raw_phdrs)
{
    const Elf64_Ehdr *header = raw_header;
    const Elf64_Phdr *phdrs = raw_phdrs;
    if (memcmp(header->e_ident, "\177ELF\2\1\1", 7) || header->e_type != ET_DYN || header->e_machine != EM_AARCH64 ||
        header->e_phentsize != sizeof(*phdrs) || !header->e_phnum || header->e_phnum > 128 || offset % PAGE)
        return 0;
    // Bionic CFI assigns one library to each 256 KiB shadow block.
    unsigned long span = 0, alignment = 1UL << 18;
    for (unsigned i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;
        unsigned long align = phdrs[i].p_align;
        if ((align && (align & (align - 1))) || align > 2 * 1024 * 1024 || phdrs[i].p_filesz > phdrs[i].p_memsz)
            return 0;
        if (align > alignment)
            alignment = align;
        if (phdrs[i].p_vaddr > NCE_EXEC_DATA_OFFSET || phdrs[i].p_memsz > NCE_EXEC_DATA_OFFSET - phdrs[i].p_vaddr ||
            (phdrs[i].p_vaddr - phdrs[i].p_offset) % PAGE)
            return 0;
        unsigned long end = (phdrs[i].p_vaddr + phdrs[i].p_memsz + PAGE - 1) & ~(PAGE - 1);
        if (end > span)
            span = end;
    }
    for (unsigned i = 0; i < layout_count; ++i)
        if (offset == layouts[i].offset && !strcmp(path, layouts[i].path))
            return span == layouts[i].span && !(layouts[i].base % alignment) ? layouts[i].base : 0;
    unsigned long start = (base + cursor + alignment - 1) & ~(alignment - 1);
    if (!span || start - base > NCE_EXEC_DATA_OFFSET || span > NCE_EXEC_DATA_OFFSET - (start - base) || layout_count == 64)
        return 0;
    unsigned index = layout_count++;
    layouts[index].path = strdup(path);
    REQUIRE(layouts[index].path);
    layouts[index].offset = offset;
    layouts[index].base = start;
    layouts[index].span = span;
    cursor = start - base + span;
    printf("{\"phase\":\"runtime_elf\",\"path\":\"%s\",\"span_bytes\":%lu}\n", path, span);
    return layouts[index].base;
}

static unsigned long elf_offset(unsigned long address)
{
    for (unsigned i = 0; i < layout_count; ++i)
        if (address >= layouts[i].base && address - layouts[i].base < layouts[i].span)
            return layouts[i].offset;
    return 0;
}

static int user_reserved(unsigned long address, unsigned long size)
{
    (void)address;
    (void)size;
    return 0;
}

unsigned long translation_linux_setup(void *kernel, unsigned long copy_user_offset,
                                      unsigned long translate_offset, struct nce_translation_state *state)
{
    REQUIRE((translate_offset != 0) == (state != NULL));
    translation = state;
    _Static_assert(NCE_TRANSLATION_CODE_WORDS * 64 == NCE_RAM_SIZE / PAGE, "RAM page bitmap size");
    const char *invalidation = getenv("NCE_TRANSLATION_CACHE");
    REQUIRE(!invalidation || !strcmp(invalidation, "full") || !strcmp(invalidation, "pages"));
    page_invalidation = !invalidation || !strcmp(invalidation, "pages");
    const char *batch_text = getenv("NCE_TRANSLATION_BATCH");
    char *end = NULL;
    instruction_batch = batch_text ? strtoul(batch_text, &end, 10) : 50000;
    REQUIRE(!batch_text || (batch_text[0] >= '0' && batch_text[0] <= '9' && !*end));
    REQUIRE(instruction_batch >= 1000 && instruction_batch <= 1000000);
    const char *preemption = getenv("NCE_TRANSLATION_PREEMPTION");
    REQUIRE(!preemption || !strcmp(preemption, "instructions") || !strcmp(preemption, "timer"));
    timer_mode = preemption && !strcmp(preemption, "timer");
    printf("{\"config\":\"translation_preemption\",\"mode\":\"%s\",\"instructions\":%lu,\"timer_interval_ns\":2000000}\n",
           timer_mode ? "timer" : "instructions", instruction_batch);
    base = (uintptr_t)mmap(NULL, RESERVATION, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE((void *)base != MAP_FAILED && base + RESERVATION < NCE_HOST_ADDRESS_LIMIT);
    if (!translation) {
        pages = calloc(NCE_USER_SIZE / PAGE, sizeof(*pages));
        REQUIRE(pages);
    }
    run_syscall = kernel + NCE_RUN_SYSCALL;
    page_fault = kernel + NCE_PAGE_FAULT;
    user_return = kernel + NCE_USER_RETURN;
    user_fault = kernel + NCE_USER_FAULT;
    user_preempt = kernel + NCE_USER_PREEMPT;
    prepare_user = kernel + NCE_PREPARE_USER;
    copy_user = kernel + copy_user_offset;
    translate_user = translate_offset ? kernel + translate_offset : NULL;
    checked(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine));
    checked(uc_ctl_set_cpu_model(engine, UC_CPU_ARM64_MAX));
    checked(uc_ctl_set_page_size(engine, PAGE));
    checked(uc_ctl_tlb_mode(engine, UC_TLB_VIRTUAL));
    const char *cache_text = getenv("NCE_TRANSLATION_CACHE_MIB");
    unsigned long cache_mib = cache_text ? strtoul(cache_text, &end, 10) : 16;
    REQUIRE(!cache_text || (cache_text[0] >= '0' && cache_text[0] <= '9' && !*end));
    REQUIRE(cache_mib == 16 || cache_mib == 64 || cache_mib == 128);
    checked(uc_ctl_set_tcg_buffer_size(engine, cache_mib * 1024 * 1024));
    const char *fastpath = getenv("NCE_TRANSLATION_LINUX_FASTPATH");
    REQUIRE(!fastpath || !strcmp(fastpath, "0") || !strcmp(fastpath, "1"));
    checked(nce_uc_set_linux_memory(engine, fastpath && !strcmp(fastpath, "1")));
    printf("{\"config\":\"translation_cache_settings\",\"cache_bytes\":%lu,\"invalidation\":\"%s\",\"linux_fastpath\":%s}\n",
           cache_mib * 1024 * 1024, page_invalidation ? "pages" : "full",
           fastpath && !strcmp(fastpath, "1") ? "true" : "false");
    checked(uc_context_alloc(engine, &empty_cpu));
    checked(uc_context_save(engine, empty_cpu));
    uc_hook hook;
    checked(uc_hook_add(engine, &hook, UC_HOOK_TLB_FILL, fill, NULL, 1, 0));
    checked(uc_hook_add(engine, &hook, UC_HOOK_INTR, interrupt, NULL, 1, 0));
    if (timer_mode)
        REQUIRE(pthread_create(&timer_thread, NULL, preempt_timer, NULL) == 0);
    nce_lkl_host.shmem_init = shmem_init;
    nce_lkl_host.shmem_reclaim = shmem_reclaim;
    nce_lkl_host.shmem_mmap = shmem_map;
    nce_lkl_host.munmap = revoke_pages;
    nce_lkl_host.user_clone = user_clone;
    nce_lkl_host.user_free = user_free;
    nce_lkl_host.user_switch = user_switch;
    nce_lkl_host.user_run = user_run;
    nce_lkl_host.user_exec = user_exec;
    nce_lkl_host.user_elf = elf_layout;
    nce_lkl_host.user_elf_offset = elf_offset;
    nce_lkl_host.user_reserved = user_reserved;
    return base;
}

void *translation_linux_process(const char *path)
{
    Process *p = calloc(1, sizeof(*p));
    REQUIRE(p);
    p->path = path;
    ++allocated;
    return p;
}

void translation_linux_cleanup(void)
{
    if (timer_mode) {
        REQUIRE(pthread_mutex_lock(&timer_lock) == 0);
        REQUIRE(!timer_active);
        timer_stopping = true;
        REQUIRE(pthread_cond_signal(&timer_condition) == 0);
        REQUIRE(pthread_mutex_unlock(&timer_lock) == 0);
        REQUIRE(pthread_join(timer_thread, NULL) == 0);
    }
    printf("{\"test\":\"translated_context_cleanup\",\"passed\":%s,\"allocated\":%lu,\"freed\":%lu,\"maps\":%lu,\"revocations\":%lu,\"tlb_fills\":%lu,\"faults\":%lu,\"syscalls\":%lu,\"preemptions\":%lu}\n",
           allocated == freed ? "true" : "false", allocated, freed, maps, revocations, fills, faults, calls, preemptions);
    printf("{\"translation_cache\":\"%s\",\"full_flushes\":%lu,\"invalidated_pages\":%lu,\"tlb_flushes\":%lu}\n",
           page_invalidation ? "pages" : "full", cache_flushes, cache_pages, tlb_flushes);
    REQUIRE(allocated == freed);
    checked(uc_context_free(empty_cpu));
    checked(uc_close(engine));
    REQUIRE(munmap(ram, NCE_RAM_SIZE) == 0);
    REQUIRE(munmap((void *)base, RESERVATION) == 0);
    free(pages);
    for (unsigned i = 0; i < layout_count; ++i)
        free(layouts[i].path);
}
