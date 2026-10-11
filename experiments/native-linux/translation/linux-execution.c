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
#ifdef NCE_DIRECT_EXECUTION
#include "../direct/direct.h"
#include "code-watch.h"
static NceDirect *direct_engine;
static bool direct_pte_memory, direct_pte_refill, direct_flat, direct_code_watch;
static NceCodeWatch *code_watch;
static NceCodeWatchStats code_watch_stats;
static void (*original_page_free)(void *, unsigned long);
static int (*original_disk_io)(unsigned int, unsigned int, unsigned long long, void *, unsigned long);
static unsigned long direct_calls, direct_fallbacks, direct_code_writes;
static struct {
    uint32_t instruction;
    unsigned long count;
} direct_missing[256];
#endif

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
static unsigned long cache_flushes, cache_pages, tlb_flushes, tlb_pages, tlb_inactive_skips;
static bool page_invalidation, retain_exec_cache, page_tlb_invalidation;
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
extern uc_err nce_uc_invalidate_tlb(uc_engine *engine, uint64_t start, uint64_t length);
extern uc_err nce_uc_set_linux_memory(uc_engine *engine, bool enabled);
extern void nce_uc_cache_stats_aarch64(uc_engine *engine, uint64_t values[5]);

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
        if (timer_active && !timer_stopping) {
            checked(nce_uc_emu_stop_at_tb(engine));
#ifdef NCE_DIRECT_EXECUTION
            nce_direct_request_stop(direct_engine);
#endif
        }
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

#ifdef NCE_DIRECT_EXECUTION
static int watch_disk_io(unsigned int index, unsigned int operation, unsigned long long offset,
                         void *buffer, unsigned long size)
{
    unsigned long start = (uintptr_t)buffer - (base + NCE_USER_SIZE);
    if (code_watch && operation == 0 && size && start < NCE_RAM_SIZE) {
        REQUIRE(size <= NCE_RAM_SIZE - start);
        unsigned long first = start & ~(PAGE - 1);
        unsigned long end = (start + size + PAGE - 1) & ~(PAGE - 1);
        /* Kernel copyout from pread reports EFAULT instead of a user write fault. */
        REQUIRE(nce_code_watch_invalidate(code_watch, first, end - first) == 0);
    }
    return original_disk_io(index, operation, offset, buffer, size);
}

static void watch_page_free(void *address, unsigned long size)
{
    if (code_watch && address == (void *)(base + NCE_USER_SIZE)) {
        /* LKL calls this after joining all task threads and before unmapping RAM. */
        REQUIRE(size == NCE_RAM_SIZE);
        code_watch_stats = nce_code_watch_stats(code_watch);
        nce_code_watch_destroy(code_watch);
        code_watch = NULL;
    }
    original_page_free(address, size);
}
#endif

static void shmem_init(unsigned long size)
{
    REQUIRE(size == NCE_RAM_SIZE);
    ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(ram != MAP_FAILED);
    if (translation)
        translation->ram = (uintptr_t)ram;
    checked(uc_mem_map_ptr(engine, 0, size, UC_PROT_ALL, ram));
#ifdef NCE_DIRECT_EXECUTION
    if (direct_code_watch) {
        code_watch = nce_code_watch_create(ram, (void *)(base + NCE_USER_SIZE), size);
        REQUIRE(code_watch);
    }
#endif
}

static int shmem_reclaim(unsigned long offset, unsigned long size)
{
    REQUIRE(!(offset % PAGE) && !(size % PAGE) && offset <= NCE_RAM_SIZE && size <= NCE_RAM_SIZE - offset);
#ifdef NCE_DIRECT_EXECUTION
    if (code_watch)
        REQUIRE(nce_code_watch_invalidate(code_watch, offset, size) == 0);
#endif
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
#ifdef NCE_DIRECT_EXECUTION
        if (code_watch) {
            if (start == base + NCE_USER_SIZE + offset)
                REQUIRE(nce_code_watch_invalidate(code_watch, offset, size) == 0);
            else
                REQUIRE(nce_code_watch_exclude(code_watch, offset, size) == 0);
        }
#endif
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

#ifdef NCE_DIRECT_EXECUTION
static unsigned direct_setting(const char *name, unsigned fallback)
{
    const char *text = getenv(name);
    if (!text)
        return fallback;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    REQUIRE(*text >= '0' && *text <= '9' && !*end && value <= UINT32_MAX);
    return (unsigned)value;
}

static void *direct_map_page(void *opaque, uint64_t address, bool write)
{
    (void)opaque;
    ++fills;
    unsigned permission = write ? LKL_PROT_WRITE : LKL_PROT_READ;
    unsigned long value = translate_user(address, permission);
    if (!(value & permission))
        return NULL;
    REQUIRE((value & ~(PAGE - 1)) < NCE_RAM_SIZE);
    return ram + (value & ~(PAGE - 1)) + (address & (PAGE - 1));
}

static const void *direct_map_code(void *opaque, uint64_t address)
{
    (void)opaque;
    unsigned long value = translate_user(address, LKL_PROT_EXEC);
    if (!(value & LKL_PROT_EXEC))
        return NULL;
    REQUIRE((value & ~(PAGE - 1)) < NCE_RAM_SIZE);
    return ram + (value & ~(PAGE - 1)) + (address & (PAGE - 1));
}

static uint64_t direct_code_version(void *opaque, const void *address)
{
    (void)opaque;
    return nce_code_watch_version(code_watch, address);
}

/* Unsupported instructions continue in Unicorn until the next Linux return.
 * Keeping the whole remainder there preserves its exclusive monitor. */
static bool direct_execute(Process *p, unsigned long generation)
{
    NceDirectState state = {0};
    memcpy(state.x, p->regs.x, sizeof(state.x));
    memcpy(state.v, p->regs.vector, sizeof(state.v));
    state.pc = p->regs.pc;
    state.sp = p->regs.sp;
    state.nzcv = p->regs.nzcv;
    state.fpcr = p->regs.fpcr;
    state.fpsr = p->regs.fpsr;
    state.tpidr_el0 = p->tls;
    state.exclusive_address = UINT64_MAX;
    state.access_flags = NCE_DIRECT_UNALIGNED | NCE_DIRECT_PAC_HINT_NOP | NCE_DIRECT_CRC |
                         NCE_DIRECT_LSE | NCE_DIRECT_AES | NCE_DIRECT_SHA1 | NCE_DIRECT_SHA256 | NCE_DIRECT_DC_ZVA64;
    NceDirectPageTable table = {(const uint64_t *)translation->pgd, base, NCE_USER_SIZE,
                                base + NCE_USER_SIZE, ram, NCE_RAM_SIZE};
    NceDirectMemory memory = {NULL, direct_map_page, direct_map_code, generation, 14,
                              direct_pte_memory ? &table : NULL, direct_pte_refill,
                              direct_flat ? (const struct nce_direct_flat_page *)translation->flat : NULL,
                              direct_code_watch ? direct_code_version : NULL};
    REQUIRE(!direct_flat || memory.flat_pages);
    uint64_t address = 0;
    ++direct_calls;
    NceDirectExit result = nce_direct_run(direct_engine, &state, memory,
                                          timer_mode ? (UINT64_C(1) << 60) : instruction_batch, &address);
    memcpy(p->regs.x, state.x, sizeof(state.x));
    memcpy(p->regs.vector, state.v, sizeof(state.v));
    p->regs.pc = state.pc;
    p->regs.sp = state.sp;
    p->regs.nzcv = state.nzcv;
    p->regs.fpcr = state.fpcr;
    p->regs.fpsr = state.fpsr;
    if (result == NCE_DIRECT_BUDGET)
        return true;
    if (result == NCE_DIRECT_FETCH_FAULT || result == NCE_DIRECT_READ_FAULT || result == NCE_DIRECT_WRITE_FAULT) {
        // A writable code alias is executed by Unicorn after ending this slice.
        if (result != NCE_DIRECT_WRITE_FAULT || !direct_map_page(NULL, address & ~UINT64_C(4095), true)) {
            fault_event(address, result == NCE_DIRECT_FETCH_FAULT ? UC_MEM_FETCH : result == NCE_DIRECT_WRITE_FAULT ? UC_MEM_WRITE
                                                                                                                    : UC_MEM_READ);
            return true;
        }
        ++direct_code_writes;
    } else
        REQUIRE(result == NCE_DIRECT_UNSUPPORTED);
    const uint32_t *code = direct_map_code(NULL, state.pc & ~UINT64_C(4095));
    REQUIRE(code);
    uint32_t word = code[(state.pc & 4095) / 4];
    if ((word & 0xffe0001f) == 0xd4000001) {
        p->regs.pc += 4;
        event.kind = 1;
        return true;
    }
    ++direct_fallbacks;
    unsigned slot = ((word >> 2) ^ (word >> 16)) % 256;
    for (unsigned i = 0; i < 256; ++i, slot = (slot + 1) % 256) {
        if (!direct_missing[slot].count || direct_missing[slot].instruction == word) {
            direct_missing[slot].instruction = word;
            ++direct_missing[slot].count;
            break;
        }
    }
    return false;
}
#endif

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
    if (!retain_exec_cache)
        ++code_generation;
}

static void prepare_unicorn(unsigned long current_maps)
{
    /* Each entry starts with no exclusive reservation. Linux may have
     * switched threads or installed a signal frame during the previous exit. */
    checked(uc_context_restore(engine, empty_cpu));
    static unsigned long seen_maps, seen_code, seen_kernel_code;
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
        unsigned long length = translation ? translation->tlb_end - translation->tlb_start : 0;
        if (page_tlb_invalidation && !length) {
            ++tlb_inactive_skips;
        } else if (page_tlb_invalidation && length <= 64 * PAGE) {
            checked(nce_uc_invalidate_tlb(engine, translation->tlb_start, length));
            tlb_pages += length / PAGE;
        } else {
            checked(uc_ctl_flush_tlb(engine));
            ++tlb_flushes;
        }
        if (translation)
            translation->tlb_start = translation->tlb_end = 0;
        seen_maps = current_maps;
    }
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
        unsigned long current_maps = translation ? translation->mappings : map_generation;
        memset(&event, 0, sizeof(event));
        bool handled = false;
#ifdef NCE_DIRECT_EXECUTION
        mark_executing(true);
        handled = direct_execute(p, current_maps);
        mark_executing(false);
#endif
        uc_err error = UC_ERR_OK;
        if (!handled) {
            prepare_unicorn(current_maps);
            registers(p, false);
            mark_executing(true);
            error = uc_emu_start(engine, p->regs.pc, 0, 0, timer_mode ? 0 : instruction_batch);
            mark_executing(false);
            registers(p, true);
        }
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

/* Runtime ELF placement retains original code and headers. The execution address
 * range and minimum library alignment bound the number of table entries. */
typedef struct {
    char *path;
    unsigned long offset, base, span;
} ElfLayout;
static ElfLayout *layouts;
static unsigned layout_count, layout_capacity;
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
    if (!span || start - base > NCE_EXEC_DATA_OFFSET || span > NCE_EXEC_DATA_OFFSET - (start - base))
        return 0;
    if (layout_count == layout_capacity) {
        unsigned capacity = layout_capacity ? layout_capacity * 2 : 16;
        REQUIRE(capacity <= NCE_EXEC_DATA_OFFSET / (1UL << 18));
        ElfLayout *grown = realloc(layouts, capacity * sizeof(*layouts));
        REQUIRE(grown);
        layouts = grown;
        layout_capacity = capacity;
    }
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
    const char *exec_cache = getenv("NCE_TRANSLATION_EXEC_CACHE");
    REQUIRE(!exec_cache || !strcmp(exec_cache, "full") || !strcmp(exec_cache, "pages"));
    retain_exec_cache = exec_cache && !strcmp(exec_cache, "pages");
    // The PTE backend reports physical code pages on install, removal and reuse.
    REQUIRE(!retain_exec_cache || (translation && page_invalidation));
    printf("{\"config\":\"translation_exec_cache\",\"mode\":\"%s\"}\n", retain_exec_cache ? "pages" : "full");
    const char *tlb = getenv("NCE_TRANSLATION_TLB");
    REQUIRE(!tlb || !strcmp(tlb, "full") || !strcmp(tlb, "pages"));
    page_tlb_invalidation = tlb && !strcmp(tlb, "pages");
    REQUIRE(!page_tlb_invalidation || translation);
    printf("{\"config\":\"translation_tlb_invalidation\",\"mode\":\"%s\",\"range_limit_pages\":64}\n",
           page_tlb_invalidation ? "pages" : "full");
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
    const char *cache_text = getenv("NCE_TRANSLATION_CACHE_MIB");
    unsigned long cache_mib = cache_text ? strtoul(cache_text, &end, 10) : 16;
    REQUIRE(!cache_text || (cache_text[0] >= '0' && cache_text[0] <= '9' && !*end));
    REQUIRE(cache_mib == 16 || cache_mib == 64 || cache_mib == 128);
    unsigned long cache_bytes = cache_mib * 1024 * 1024;
#ifdef NCE_DIRECT_EXECUTION
    REQUIRE(translation && translate_user);
    unsigned selective = direct_setting("NCE_DIRECT_SELECTIVE_WRITES", 0);
    unsigned reuse = direct_setting("NCE_DIRECT_REUSE_CODE_CHECKS", 0);
    unsigned watch = direct_setting("NCE_DIRECT_CODE_WATCH", 0);
    unsigned crossing = direct_setting("NCE_DIRECT_CROSS_PAGE", 0);
    REQUIRE(watch <= 1 && crossing <= 1);
    direct_code_watch = watch;
    const char *direct_memory = getenv("NCE_DIRECT_MEMORY");
    REQUIRE(!direct_memory || !strcmp(direct_memory, "cached") || !strcmp(direct_memory, "pte") || !strcmp(direct_memory, "refill") || !strcmp(direct_memory, "flat"));
    direct_pte_memory = direct_memory && strcmp(direct_memory, "cached");
    direct_pte_refill = direct_memory && !strcmp(direct_memory, "refill");
    direct_flat = direct_memory && !strcmp(direct_memory, "flat");
    translation->flat_enabled = direct_flat;
    REQUIRE(selective <= 1 && reuse <= 1);
    NceDirectConfig direct_config = {cache_bytes, direct_setting("NCE_DIRECT_MAPPING_ENTRIES", 256),
                                     direct_setting("NCE_DIRECT_ROUTE_ENTRIES", 256), selective != 0, reuse != 0, crossing != 0};
    REQUIRE(!direct_pte_memory || direct_pte_refill || (direct_config.mapping_entries == 256 && !selective));
    direct_engine = nce_direct_create(direct_config);
    REQUIRE(direct_engine);
    printf("{\"config\":\"translation_execution\",\"engine\":\"direct\",\"direct_cache_bytes\":%lu,\"data_page_bytes\":16384,\"mapping_entries\":%u,\"route_entries\":%u,\"selective_writes\":%s,\"reuse_code_checks\":%s,\"memory\":\"%s\",\"code_watch\":%s,\"cross_page\":%s}\n",
           cache_bytes, direct_config.mapping_entries, direct_config.route_entries, direct_config.selective_writes ? "true" : "false", direct_config.reuse_code_checks ? "true" : "false", direct_memory ? direct_memory : "cached", direct_code_watch ? "true" : "false", crossing ? "true" : "false");
    if (direct_code_watch) {
        original_page_free = nce_lkl_host.page_free;
        nce_lkl_host.page_free = watch_page_free;
        original_disk_io = nce_lkl_host.disk_io;
        nce_lkl_host.disk_io = watch_disk_io;
    }
#else
    printf("{\"config\":\"translation_execution\",\"engine\":\"unicorn\"}\n");
#endif
    // TLB mode initializes Unicorn; buffer size must be supplied before that call.
    checked(uc_ctl_set_tcg_buffer_size(engine, (uint32_t)cache_bytes));
    checked(uc_ctl_tlb_mode(engine, UC_TLB_VIRTUAL));
    const char *fastpath = getenv("NCE_TRANSLATION_LINUX_FASTPATH");
    REQUIRE(!fastpath || !strcmp(fastpath, "0") || !strcmp(fastpath, "1"));
    checked(nce_uc_set_linux_memory(engine, fastpath && !strcmp(fastpath, "1")));
    uint64_t stats[5];
    nce_uc_cache_stats_aarch64(engine, stats);
    REQUIRE(stats[4] <= cache_bytes && cache_bytes - stats[4] < 65536);
    printf("{\"config\":\"translation_cache_settings\",\"cache_bytes\":%lu,\"actual_cache_bytes\":%llu,\"invalidation\":\"%s\",\"linux_fastpath\":%s}\n",
           cache_bytes, stats[4], page_invalidation ? "pages" : "full",
           fastpath && !strcmp(fastpath, "1") ? "true" : "false");
#ifdef NCE_DIRECT_EXECUTION
    uc_arm64_cp_reg dczid = {.op0 = 3, .op1 = 3, .crn = 0, .crm = 0, .op2 = 7};
    checked(uc_reg_read(engine, UC_ARM64_REG_CP_REG, &dczid));
    REQUIRE(dczid.val == 4);
#endif
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
    printf("{\"translation_tlb\":\"%s\",\"full_flushes\":%lu,\"invalidated_pages\":%lu,\"inactive_skips\":%lu,\"mm_switches\":%lu,\"inactive_mapping_changes\":%lu}\n",
           page_tlb_invalidation ? "pages" : "full", tlb_flushes, tlb_pages, tlb_inactive_skips,
           translation ? translation->mm_switches : 0, translation ? translation->inactive_mapping_changes : 0);
    uint64_t stats[5];
    nce_uc_cache_stats_aarch64(engine, stats);
    printf("{\"translation_engine_cache\":true,\"total_flushes\":%llu,\"invalidated_blocks\":%llu,\"current_blocks\":%llu,\"used_bytes\":%llu,\"capacity_bytes\":%llu}\n",
           stats[0], stats[1], stats[2], stats[3], stats[4]);
#ifdef NCE_DIRECT_EXECUTION
    REQUIRE(!code_watch);
    printf("{\"direct_code_watch\":true,\"enabled\":%s,\"queries\":%llu,\"protections\":%llu,\"write_faults\":%llu,\"invalidations\":%llu,\"protection_failures\":%llu,\"excluded_pages\":%llu}\n",
           direct_code_watch ? "true" : "false", code_watch_stats.queries, code_watch_stats.protections,
           code_watch_stats.write_faults, code_watch_stats.invalidations, code_watch_stats.protection_failures, code_watch_stats.excluded_pages);
    printf("{\"direct_flat_tables\":true,\"enabled\":%s,\"live_virtual_bytes\":%lu,\"peak_virtual_bytes\":%lu,\"updates\":%lu,\"refresh_pages\":%lu}\n",
           direct_flat ? "true" : "false", translation->flat_live_bytes, translation->flat_peak_bytes,
           translation->flat_updates, translation->flat_refresh_pages);
    REQUIRE(!translation->flat_live_bytes);
    NceDirectStats direct = nce_direct_stats(direct_engine);
    printf("{\"direct_code_validation\":true,\"content_checks\":%llu,\"version_hits\":%llu}\n", direct.code_content_checks, direct.code_version_hits);
    printf("{\"direct_fallback_reasons\":true,\"alignment\":%llu,\"cross_page\":%llu,\"code_write\":%lu}\n",
           direct.alignment_exits, direct.cross_page_fallbacks, direct_code_writes);
    printf("{\"direct_cross_page\":true,\"exits\":%llu,\"completed\":%llu,\"fallbacks\":%llu,\"faults\":%llu,\"bytes\":%llu}\n",
           direct.cross_page_exits, direct.cross_page_completed, direct.cross_page_fallbacks, direct.cross_page_faults, direct.cross_page_bytes);
    printf("{\"direct_execution\":true,\"calls\":%lu,\"fallbacks\":%lu,\"compiles\":%llu,\"flushes\":%llu,\"code_changes\":%llu,\"code_bytes\":%zu,\"capacity_bytes\":%zu,\"pages\":%zu,\"mapping_misses\":%llu,\"code_lookups\":%llu,\"read_mapping_misses\":%llu,\"write_mapping_misses\":%llu,\"write_invalidations\":%llu,\"code_validation_hits\":%llu,\"pte_read_refills\":%llu}\n",
           direct_calls, direct_fallbacks, direct.compiles, direct.flushes, direct.code_changes,
           direct.code_bytes, direct.code_capacity, direct.pages, direct.mapping_misses, direct.code_lookups,
           direct.read_mapping_misses, direct.write_mapping_misses, direct.write_invalidations, direct.code_validation_hits, direct.pte_read_refills);
    for (unsigned i = 0; i < 256; ++i)
        if (direct_missing[i].count)
            printf("{\"direct_fallback\":%u,\"count\":%lu}\n", direct_missing[i].instruction, direct_missing[i].count);
    nce_direct_destroy(direct_engine);
#endif
    REQUIRE(allocated == freed);
    checked(uc_context_free(empty_cpu));
    checked(uc_close(engine));
    REQUIRE(munmap(ram, NCE_RAM_SIZE) == 0);
    REQUIRE(munmap((void *)base, RESERVATION) == 0);
    free(pages);
    for (unsigned i = 0; i < layout_count; ++i)
        free(layouts[i].path);
    free(layouts);
}
