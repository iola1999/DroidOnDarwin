/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "linux-execution.h"
#include "interaction-profile.h"
#include "guest-registers.h"
#include "layout.h"
#include "lkl-host-ops.h"
#include "lkl-image.h"
#include "linux-elf.h"
#ifdef NCE_TRANSLATION_DUAL_MAP
#include "unicorn-code-memory.h"
#endif
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unicorn/unicorn.h>
#ifdef NCE_DIRECT_EXECUTION
#include "../direct/direct.h"
#include "code-watch.h"
#include "read-view.h"
static bool direct_pte_memory, direct_pte_refill, direct_flat, direct_code_watch;
static bool direct_read_views;
static unsigned long read_views_created, read_views_destroyed, read_views_live, read_views_peak, read_view_maps, read_view_revocations;
static NceCodeWatch *code_watch;
static NceCodeWatchStats code_watch_stats;
static void (*original_page_free)(void *, unsigned long);
static int (*original_disk_io)(unsigned int, unsigned int, unsigned long long, void *, unsigned long);
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
    NceExecutionProfile *profile;
    uint32_t fallback_instruction;
} Process;
static unsigned char *ram;
static unsigned long guest_base, guest_end, physical_base, reservation_base, reservation_size, *pages;
static bool low_guest_layout;
static unsigned long map_generation, code_generation, maps, revocations;
static unsigned long allocated, freed;
static bool page_invalidation, retain_exec_cache, page_tlb_invalidation;
static unsigned long instruction_batch;
static bool timer_mode, timer_started, timer_stopping, parallel_profile;
static uint64_t profile_last_ns, profile_union_ns, profile_overlap_ns;
static pthread_t timer_thread;
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_condition = PTHREAD_COND_INITIALIZER;
static long (*run_syscall)(long, long *);
static int (*page_fault)(unsigned long, int);
static void (*user_return)(long, long, unsigned long), (*user_fault)(unsigned long, int);
static void (*user_preempt)(void), (*prepare_user)(void);
static unsigned long (*copy_user)(void *, unsigned long, unsigned long, int);
static unsigned long (*translate_user)(unsigned long, unsigned long);
static struct nce_translation_state *translation;
#ifdef NCE_SMP
static unsigned long (*enter_guest)(void);
static void (*leave_guest)(unsigned long), (*exclusive_begin)(void), (*exclusive_end)(void);
static void (*kernel_written)(unsigned long, unsigned long);

#endif
#ifndef NCE_TRANSLATION_CPUS
#define NCE_TRANSLATION_CPUS 1
#endif
static const unsigned execution_count = NCE_TRANSLATION_CPUS;
typedef struct {
    unsigned index;
    struct nce_translation_cpu *view;
    uc_engine *engine;
    uc_context *empty_cpu;
    bool timer_active;
    uint64_t run_started_cpu_ns, run_cpu_ns;
    unsigned long fills, faults, calls, preemptions;
    unsigned long cache_flushes, cache_pages, tlb_flushes, tlb_pages, tlb_inactive_skips;
    unsigned long icache_operations, icache_invalidations;
    unsigned long seen_maps, seen_code, seen_kernel_code;
    struct {
        int kind, access;
        uint64_t address;
    } event;
#ifdef NCE_DIRECT_EXECUTION
    NceDirect *direct_engine;
    unsigned long direct_calls, direct_fallbacks, direct_code_writes;
    struct {
        uint32_t instruction;
        unsigned long count;
    } direct_missing[256];
#endif
} ExecutionCpu;
static ExecutionCpu executions[NCE_TRANSLATION_MAX_CPUS];
/* Linux invokes user_switch after scheduling, including task migration. */
static _Thread_local ExecutionCpu *execution = &executions[0];
#ifdef NCE_SMP
/* IRQ-disabled registration retains all directory pages. Read each parent
 * once because Linux may clear a parent before its RCU reclamation. */
static unsigned long lookup_guest(unsigned long address, unsigned long permission)
{
    if (address < guest_base || address >= guest_end)
        return 0;
    const unsigned long *table = (void *)execution->view->pgd;
    unsigned shifts[] = {36, 25, 14};
    if (!table)
        return 0;
    unsigned long value = 0;
    for (unsigned i = 0; i < 3; ++i) {
        value = __atomic_load_n(&table[(address >> shifts[i]) & 2047], __ATOMIC_ACQUIRE);
        if (!(value & 1))
            return 0;
        if (i != 2)
            table = (void *)(value & ~(PAGE - 1));
    }
    if ((value & 0x0d1) != 0x0c1)
        return 0;
    unsigned long offset = (value & ~(PAGE - 1)) - physical_base;
    if (offset >= NCE_RAM_SIZE)
        return 0;
    unsigned long prot = LKL_PROT_READ;
    if ((value & 0x120) == 0x120)
        prot |= LKL_PROT_WRITE;
    if (value & 0x200)
        prot |= LKL_PROT_EXEC;
    return prot & permission ? offset | prot : 0;
}

static void user_prepare(unsigned index)
{
    REQUIRE(index < execution_count);
    execution = &executions[index];
    nce_direct_prepare_run(execution->direct_engine);
}

static void user_interrupt(unsigned mask)
{
    for (unsigned i = 0; i < execution_count; ++i)
        if (mask & (1U << i))
            nce_direct_request_stop(executions[i].direct_engine);
}
#endif

extern struct lkl_host_operations nce_lkl_host;
extern uc_err nce_uc_emu_stop_at_tb(uc_engine *uc);
extern uc_err nce_uc_invalidate_ram(uc_engine *uc, uint64_t start, uint64_t length);
extern uc_err nce_uc_invalidate_tlb(uc_engine *uc, uint64_t start, uint64_t length);
extern uc_err nce_uc_set_linux_memory(uc_engine *uc, bool enabled);
extern void nce_uc_cache_stats_aarch64(uc_engine *uc, uint64_t values[5]);

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
        for (unsigned i = 0; i < execution_count && !timer_stopping; ++i) {
            ExecutionCpu *execution = &executions[i];
            if (!execution->timer_active)
                continue;
            checked(nce_uc_emu_stop_at_tb(execution->engine));
#ifdef NCE_DIRECT_EXECUTION
            nce_direct_request_stop(execution->direct_engine);
#endif
        }
    }
    REQUIRE(pthread_mutex_unlock(&timer_lock) == 0);
    return NULL;
}

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec value;
    REQUIRE(!clock_gettime(clock, &value));
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void mark_executing(bool active)
{
    if (!timer_mode && !parallel_profile)
        return;
    REQUIRE(pthread_mutex_lock(&timer_lock) == 0);
    if (parallel_profile) {
        uint64_t now = clock_ns(CLOCK_MONOTONIC);
        unsigned running = 0;
        for (unsigned i = 0; i < execution_count; ++i)
            running += executions[i].timer_active;
        if (running)
            profile_union_ns += now - profile_last_ns;
        if (running > 1)
            profile_overlap_ns += now - profile_last_ns;
        profile_last_ns = now;
        uint64_t thread = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        if (active)
            execution->run_started_cpu_ns = thread;
        else
            execution->run_cpu_ns += thread - execution->run_started_cpu_ns;
    }
    execution->timer_active = active;
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
static void *read_view_create(unsigned long size)
{
    NceReadView *view = calloc(1, sizeof(*view));
    if (!view)
        return NULL;
    if (nce_read_view_create(view, size)) {
        free(view);
        return NULL;
    }
    ++read_views_created;
    read_views_live += size;
    if (read_views_live > read_views_peak)
        read_views_peak = read_views_live;
    return view;
}

static void read_view_update(void *opaque, unsigned long offset, unsigned long source)
{
    NceReadView *view = opaque;
    REQUIRE(!(source ? nce_read_view_map(view, offset, (void *)source, PAGE)
                     : nce_read_view_revoke(view, offset, PAGE)));
}

static void read_view_destroy(void *opaque)
{
    NceReadView *view = opaque;
    read_view_maps += view->maps;
    read_view_revocations += view->revocations;
    read_views_live -= view->size;
    REQUIRE(!nce_read_view_destroy(view));
    if (execution->view->read_view == view)
        execution->view->read_view = NULL;
    free(view);
    ++read_views_destroyed;
}

static int watch_disk_io(unsigned int index, unsigned int operation, unsigned long long offset,
                         void *buffer, unsigned long size)
{
    unsigned long start = (uintptr_t)buffer - physical_base;
#ifdef NCE_SMP
    bool writing = operation == 0 && size && start < NCE_RAM_SIZE;
    if (writing)
        exclusive_begin();
#endif
    if (code_watch && operation == 0 && size && start < NCE_RAM_SIZE) {
        REQUIRE(size <= NCE_RAM_SIZE - start);
        unsigned long first = start & ~(PAGE - 1);
        unsigned long end = (start + size + PAGE - 1) & ~(PAGE - 1);
        /* Kernel copyout from pread reports EFAULT instead of a user write fault. */
        REQUIRE(nce_code_watch_invalidate(code_watch, first, end - first) == 0);
    }
#ifdef NCE_SMP
    int result = original_disk_io(index, operation, offset, buffer, size);
    if (writing) {
        kernel_written((uintptr_t)buffer, size);
        exclusive_end();
    }
    return result;
#else
    return original_disk_io(index, operation, offset, buffer, size);
#endif
}

static void watch_page_free(void *address, unsigned long size)
{
    if (code_watch && address == (void *)physical_base) {
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
    printf("{\"config\":\"translation_guest_layout\",\"mode\":\"%s\",\"guest_base\":%lu,\"guest_end\":%lu,\"guest_span_bytes\":%lu,\"flat_coverage_bytes\":%lu,\"physical_base\":%lu,\"ram_base\":%lu,\"ram_bytes\":%lu,\"reservation_base\":%lu,\"reservation_bytes\":%lu}\n",
           low_guest_layout ? "low" : "host", guest_base, guest_end, guest_end - guest_base,
           translation && translation->flat_enabled ? NCE_USER_SIZE : 0UL, physical_base, (unsigned long)ram, NCE_RAM_SIZE, reservation_base, reservation_size);
    for (unsigned i = 0; i < execution_count; ++i)
        checked(uc_mem_map_ptr(executions[i].engine, 0, size, UC_PROT_ALL, ram));
#ifdef NCE_DIRECT_EXECUTION
    if (direct_code_watch) {
        code_watch = nce_code_watch_create(ram, (void *)physical_base, size);
        REQUIRE(code_watch);
    }
#endif
}

static int shmem_reclaim(unsigned long offset, unsigned long size)
{
#ifdef NCE_SMP
    exclusive_begin();
#endif
    REQUIRE(!(offset % PAGE) && !(size % PAGE) && offset <= NCE_RAM_SIZE && size <= NCE_RAM_SIZE - offset);
#ifdef NCE_DIRECT_EXECUTION
    if (code_watch)
        REQUIRE(nce_code_watch_invalidate(code_watch, offset, size) == 0);
#endif
    REQUIRE(mmap(ram + offset, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == ram + offset);
    remap((void *)(physical_base + offset), ram + offset, size, PROT_READ | PROT_WRITE);
    if (translation) {
        for (unsigned cpu = 0; cpu < execution_count; ++cpu) {
            ExecutionCpu *execution = &executions[cpu];
            for (unsigned long i = offset / PAGE; i < (offset + size) / PAGE; ++i)
                execution->view->code_pages[i / 64] |= 1UL << (i % 64);
            ++execution->view->code;
        }
    } else
        ++code_generation;
#ifdef NCE_SMP
    for (unsigned long page = offset / PAGE; page < (offset + size) / PAGE; ++page)
        REQUIRE(!translation->code_guard[page]);
    exclusive_end();
#endif
    return 0;
}

static void *shmem_map(void *address, unsigned long offset, unsigned long size, enum lkl_prot prot)
{
    unsigned long start = (uintptr_t)address;
    REQUIRE(!(start % PAGE) && !(offset % PAGE) && !(size % PAGE));
    REQUIRE(offset <= NCE_RAM_SIZE && size <= NCE_RAM_SIZE - offset);
    if (start >= physical_base) {
        REQUIRE(start <= reservation_base + reservation_size && size <= reservation_base + reservation_size - start);
#ifdef NCE_DIRECT_EXECUTION
        if (code_watch) {
#ifdef NCE_SMP
            exclusive_begin();
#endif
            if (start == physical_base + offset)
                REQUIRE(nce_code_watch_invalidate(code_watch, offset, size) == 0);
            else
                REQUIRE(nce_code_watch_exclude(code_watch, offset, size) == 0);
        }
#endif
        remap(address, ram + offset, size, prot);
#ifdef NCE_SMP
        if (code_watch)
            exclusive_end();
#endif
        return address;
    }
    REQUIRE(!translation);
    REQUIRE(start >= guest_base && start <= guest_end && size <= guest_end - start);
    for (unsigned long i = 0; i < size; i += PAGE) {
        unsigned long index = (start + i - guest_base) / PAGE;
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
    REQUIRE(!(start % PAGE) && !(size % PAGE));
    if (start >= guest_base && start < guest_end) {
        REQUIRE(!translation);
        unsigned long length = size < guest_end - start ? size : guest_end - start;
        for (unsigned long i = 0; i < length; i += PAGE) {
            unsigned long index = (start + i - guest_base) / PAGE;
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
    if (size) {
        REQUIRE(start >= physical_base && start <= reservation_base + reservation_size &&
                size <= reservation_base + reservation_size - start);
        REQUIRE(mmap((void *)start, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (void *)start);
    }
    return 0;
}

static void fault_event(uint64_t address, uc_mem_type access)
{
    execution->event.kind = 2;
    execution->event.address = address;
    execution->event.access = access == UC_MEM_WRITE || access == UC_MEM_WRITE_PROT || access == UC_MEM_WRITE_UNMAPPED ? 1 : access == UC_MEM_FETCH || access == UC_MEM_FETCH_PROT || access == UC_MEM_FETCH_UNMAPPED ? NCE_FAULT_EXEC
                                                                                                                                                                                                                      : 0;
}

static bool fill(uc_engine *uc, uint64_t address, uc_mem_type access, uc_tlb_entry *entry, void *opaque)
{
    (void)uc;
    (void)opaque;
    ++execution->fills;
    unsigned permission = access == UC_MEM_WRITE ? UC_PROT_WRITE : access == UC_MEM_FETCH ? UC_PROT_EXEC
                                                                                          : UC_PROT_READ;
    if (address >= guest_base && address < guest_end) {
        unsigned long value = translate_user ? translate_user(address, permission) : pages[(address - guest_base) / PAGE];
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

#ifdef NCE_SMP
static void fallback_write(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *opaque)
{
    (void)uc;
    (void)type;
    (void)value;
    (void)opaque;
    /* Unicorn memory hooks report physical addresses after virtual TLB lookup.
     * This engine maps its shared RAM block at physical address zero. */
    if (size <= 0 || address >= NCE_RAM_SIZE || (unsigned)size > NCE_RAM_SIZE - address)
        return;
    kernel_written(physical_base + address, (unsigned)size);
}
#endif

static void interrupt(uc_engine *uc, uint32_t number, void *opaque)
{
    (void)opaque;
    execution->event.kind = number == 2 ? 1 : 3;
    checked(uc_emu_stop(uc));
}

static uint32_t cache_instruction(uc_engine *uc, uc_arm64_reg reg, const uc_arm64_cp_reg *operation, void *opaque)
{
    (void)reg;
    (void)opaque;
    if (operation->op0 != 1 || operation->op1 != 3 || operation->crn != 7 ||
        operation->crm != 5 || operation->op2 != 1)
        return 0;
    ++execution->icache_operations;
    unsigned long value = translate_user(operation->val, LKL_PROT_READ);
    /* A missing PTE is installed through a later Linux fault, which already
     * invalidates its physical code page. Present aliases need an explicit
     * invalidation: kernel writes do not pass through Unicorn's guest stores.
     * SMP fallback holds the writer lock and has stopped other executors. */
    if (value & LKL_PROT_READ) {
        unsigned long offset = value & ~(PAGE - 1);
        REQUIRE(offset < NCE_RAM_SIZE);
        checked(nce_uc_invalidate_ram(uc, offset, PAGE));
        ++execution->icache_invalidations;
        for (unsigned cpu = 0; cpu < execution_count; ++cpu) {
            if (cpu == execution->index)
                continue;
            struct nce_translation_cpu *view = &translation->cpus[cpu];
            unsigned long index = offset / PAGE;
            view->code_pages[index / 64] |= 1UL << (index % 64);
            ++view->code;
        }
    }
    return 0;
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
    checked(save ? uc_reg_read_batch(execution->engine, ids, values, count) : uc_reg_write_batch(execution->engine, ids, values, count));
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
    ++execution->fills;
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
    NceDirectPageTable table = {(const uint64_t *)execution->view->pgd, guest_base, NCE_USER_SIZE,
                                physical_base, ram, NCE_RAM_SIZE};
    NceReadView *view = direct_read_views ? execution->view->read_view : NULL;
    REQUIRE(!direct_read_views || view);
    NceDirectMemory memory = {NULL, direct_map_page, direct_map_code, generation, 14,
                              direct_pte_memory ? &table : NULL, direct_pte_refill,
                              direct_flat ? (const struct nce_direct_flat_page *)execution->view->flat : NULL,
                              direct_code_watch ? direct_code_version : NULL,
                              direct_code_watch ? nce_code_watch_epoch(code_watch) : 0,
                              view ? view->base : NULL};
    REQUIRE(!direct_flat || memory.flat_pages);
    uint64_t address = 0;
    ++execution->direct_calls;
    NceDirectExit result = nce_direct_run(execution->direct_engine, &state, memory,
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
        ++execution->direct_code_writes;
    } else
        REQUIRE(result == NCE_DIRECT_UNSUPPORTED);
    const uint32_t *code = direct_map_code(NULL, state.pc & ~UINT64_C(4095));
    REQUIRE(code);
    uint32_t word = code[(state.pc & 4095) / 4];
    if ((word & 0xffe0001f) == 0xd4000001) {
        p->regs.pc += 4;
        execution->event.kind = 1;
        return true;
    }
    ++execution->direct_fallbacks;
    p->fallback_instruction = word;
    unsigned slot = ((word >> 2) ^ (word >> 16)) % 256;
    for (unsigned i = 0; i < 256; ++i, slot = (slot + 1) % 256) {
        if (!execution->direct_missing[slot].count || execution->direct_missing[slot].instruction == word) {
            execution->direct_missing[slot].instruction = word;
            ++execution->direct_missing[slot].count;
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
    p->profile = NULL;
    p->regs.x[0] = 0;
    if (sp)
        p->regs.sp = sp;
    if (flags & 0x80000)
        p->tls = tls;
    __atomic_fetch_add(&allocated, 1, __ATOMIC_RELAXED);
    return p;
}

static void user_free(void *p)
{
    __atomic_fetch_add(&freed, 1, __ATOMIC_RELAXED);
    free(p);
}

static void user_switch(void *p, unsigned index, unsigned count)
{
    (void)p;
    REQUIRE(index < execution_count && count == execution_count);
    execution = &executions[index];
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
    /* Leaving Direct may migrate the task to another CPU before the fallback
     * acquires the writer lock. Clear the event owned by the selected CPU. */
    memset(&execution->event, 0, sizeof(execution->event));
    /* Each entry starts with no exclusive reservation. Linux may have
     * switched threads or installed a signal frame during the previous exit. */
    checked(uc_context_restore(execution->engine, execution->empty_cpu));
    unsigned long current_kernel_code = translation ? execution->view->code : 0;
    if (execution->seen_code != code_generation || execution->seen_kernel_code != current_kernel_code) {
        bool all = execution->seen_code != code_generation || !page_invalidation || !translation;
        if (all) {
            checked(uc_ctl_flush_tb(execution->engine));
            ++execution->cache_flushes;
        }
        if (translation) {
            for (unsigned i = 0; i < NCE_TRANSLATION_CODE_WORDS; ++i) {
                unsigned long bits = execution->view->code_pages[i];
                while (bits && !all) {
                    unsigned bit = __builtin_ctzl(bits);
                    checked(nce_uc_invalidate_ram(execution->engine, (i * 64UL + bit) * PAGE, PAGE));
                    ++execution->cache_pages;
                    bits &= bits - 1;
                }
                execution->view->code_pages[i] = 0;
            }
        }
        execution->seen_code = code_generation;
        execution->seen_kernel_code = current_kernel_code;
    }
    if (execution->seen_maps != current_maps) {
        unsigned long length = translation ? execution->view->tlb_end - execution->view->tlb_start : 0;
        if (page_tlb_invalidation && !length) {
            ++execution->tlb_inactive_skips;
        } else if (page_tlb_invalidation && length <= 64 * PAGE) {
            checked(nce_uc_invalidate_tlb(execution->engine, execution->view->tlb_start, length));
            execution->tlb_pages += length / PAGE;
        } else {
            checked(uc_ctl_flush_tlb(execution->engine));
            ++execution->tlb_flushes;
        }
        if (translation)
            execution->view->tlb_start = execution->view->tlb_end = 0;
        execution->seen_maps = current_maps;
    }
}

static void user_run(void *opaque)
{
    Process *p = opaque;
    if (!p->regs.sp) {
        long args[6] = {guest_end - 65536, 65536, 3, 0x32, -1, 0};
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
        if (!p->profile && nce_execution_profile_enabled()) {
            long args[6] = {0};
            long pid = run_syscall(172, args), tid = run_syscall(178, args);
            p->profile = nce_execution_profile_thread(pid, tid);
        }
        NceProfileStamp stamp = nce_execution_profile_begin();
#ifdef NCE_SMP
        unsigned long irq_flags = enter_guest();
        bool exclusive = false;
#else
        prepare_user();
#endif
        nce_execution_profile_end(p->profile, NCE_STAGE_PREPARE, stamp, 0);
        unsigned long current_maps = translation ? execution->view->mappings : map_generation;
        memset(&execution->event, 0, sizeof(execution->event));
        bool handled = false;
#ifdef NCE_DIRECT_EXECUTION
        mark_executing(true);
        stamp = nce_execution_profile_begin();
        handled = direct_execute(p, current_maps);
        nce_execution_profile_end(p->profile, NCE_STAGE_DIRECT, stamp, 0);
        mark_executing(false);
#endif
        uc_err error = UC_ERR_OK;
        if (!handled) {
#ifdef NCE_SMP
            leave_guest(irq_flags);
            exclusive_begin();
            exclusive = true;
            current_maps = execution->view->mappings;
#endif
            stamp = nce_execution_profile_begin();
            prepare_unicorn(current_maps);
            registers(p, false);
            mark_executing(true);
            error = uc_emu_start(execution->engine, p->regs.pc, 0, 0, timer_mode ? 0 : instruction_batch);
            mark_executing(false);
            registers(p, true);
            nce_execution_profile_end(p->profile, NCE_STAGE_UNICORN, stamp, p->fallback_instruction);
        }
        /* Copy callback state before entering Linux: a blocking syscall or
         * page fault can let another Linux thread use this same engine. */
        int kind = execution->event.kind, access = execution->event.access;
        unsigned long address = execution->event.address;
#ifdef NCE_SMP
        if (exclusive)
            exclusive_end();
        else
            leave_guest(irq_flags);
#endif
        if (kind == 1) {
            long args[6];
            memcpy(args, p->regs.x, sizeof(args));
            long number = p->regs.x[8];
            ++execution->calls;
            long result = run_syscall(number, args);
            if (number == NCE_SYS_PUBLISH_CODE && !translation)
                ++code_generation;
            user_return(number, result, args[0]);
        } else if (kind == 2) {
            ++execution->faults;
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
            ++execution->preemptions;
            user_preempt();
        }
    }
}

static int user_reserved(unsigned long address, unsigned long size)
{
    return low_guest_layout && size &&
           (address < guest_base || address >= guest_end || size > guest_end - address);
}

unsigned long translation_linux_setup(void *kernel, unsigned long copy_user_offset,
                                      unsigned long translate_offset, struct nce_translation_state *state)
{
    REQUIRE((translate_offset != 0) == (state != NULL));
    translation = state;
    const char *layout = getenv("NCE_TRANSLATION_GUEST_LAYOUT");
    REQUIRE(!layout || !strcmp(layout, "host") || !strcmp(layout, "low"));
    low_guest_layout = layout && !strcmp(layout, "low");
    REQUIRE(!low_guest_layout || (translation && execution_count == 1));
#ifdef NCE_DIRECT_EXECUTION
    const char *layout_memory = getenv("NCE_DIRECT_MEMORY");
    REQUIRE(!low_guest_layout || (layout_memory && !strcmp(layout_memory, "flat")));
#else
    REQUIRE(!low_guest_layout);
#endif
#ifdef NCE_SMP
    REQUIRE(translation && execution_count == 2);
    enter_guest = kernel + NCE_TRANSLATION_ENTER;
    leave_guest = kernel + NCE_TRANSLATION_LEAVE;
    exclusive_begin = kernel + NCE_TRANSLATION_EXCLUSIVE;
    exclusive_end = kernel + NCE_WINDOW_UNLOCK;
    kernel_written = kernel + NCE_TRANSLATION_WRITTEN;
    nce_lkl_host.user_prepare = user_prepare;
    nce_lkl_host.user_interrupt = user_interrupt;
    translation->code_guard = calloc(NCE_RAM_SIZE / PAGE, sizeof(unsigned long));
    REQUIRE(translation->code_guard);
#endif
    if (translation)
        translation->cpu_count = execution_count;
    REQUIRE(execution_count && execution_count <= NCE_TRANSLATION_MAX_CPUS);
    for (unsigned i = 0; i < execution_count; ++i) {
        executions[i].index = i;
        executions[i].view = translation ? &translation->cpus[i] : NULL;
    }
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
    const char *profile = getenv("NCE_TRANSLATION_PARALLEL_PROFILE");
    REQUIRE(!profile || !strcmp(profile, "0") || !strcmp(profile, "1"));
    parallel_profile = profile && !strcmp(profile, "1");
    const char *access = getenv("NCE_TRANSLATION_UACCESS_STOP");
    REQUIRE(!access || !strcmp(access, "all") || !strcmp(access, "code"));
    bool code_only = access && !strcmp(access, "code");
    REQUIRE(!code_only || execution_count == 2);
    if (translation)
        translation->uaccess_code_only = code_only;
    printf("{\"config\":\"translation_parallel\",\"cpus\":%u,\"profile\":%s,\"uaccess_stop\":\"%s\"}\n",
           execution_count, parallel_profile ? "true" : "false", code_only ? "code" : "all");
    reservation_size = low_guest_layout ? NCE_RAM_SIZE + NCE_VMALLOC_SIZE : RESERVATION;
    reservation_base = (uintptr_t)mmap(NULL, reservation_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if ((void *)reservation_base == MAP_FAILED) {
        reservation_base = 0;
        goto failed;
    }
    if (reservation_base >= NCE_HOST_ADDRESS_LIMIT || reservation_size > NCE_HOST_ADDRESS_LIMIT - reservation_base)
        goto failed;
    guest_base = low_guest_layout ? 0x10000UL : reservation_base;
    guest_end = low_guest_layout ? NCE_USER_SIZE : guest_base + NCE_USER_SIZE;
    physical_base = reservation_base + (low_guest_layout ? 0 : NCE_USER_SIZE);
    if (physical_base < guest_end)
        goto failed;
    if (translation) {
        translation->guest_end = guest_end;
        translation->physical_base = physical_base;
    }
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
#ifdef NCE_SMP
    translate_user = lookup_guest;
#endif
    const char *cache_text = getenv("NCE_TRANSLATION_CACHE_MIB");
    unsigned long cache_mib = cache_text ? strtoul(cache_text, &end, 10) : 16;
    REQUIRE(!cache_text || (cache_text[0] >= '0' && cache_text[0] <= '9' && !*end));
    REQUIRE(cache_mib == 16 || cache_mib == 64 || cache_mib == 128);
    unsigned long cache_bytes = cache_mib * 1024 * 1024;
    for (unsigned cpu_index = 0; cpu_index < execution_count; ++cpu_index) {
        ExecutionCpu *execution = &executions[cpu_index];
        if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &execution->engine) != UC_ERR_OK)
            goto failed;
        if (uc_ctl_set_cpu_model(execution->engine, UC_CPU_ARM64_MAX) != UC_ERR_OK)
            goto failed;
        if (uc_ctl_set_page_size(execution->engine, PAGE) != UC_ERR_OK)
            goto failed;
#ifdef NCE_DIRECT_EXECUTION
        REQUIRE(translation && translate_user);
        unsigned selective = direct_setting("NCE_DIRECT_SELECTIVE_WRITES", 0);
        unsigned reuse = direct_setting("NCE_DIRECT_REUSE_CODE_CHECKS", 0);
        unsigned watch = direct_setting("NCE_DIRECT_CODE_WATCH", 0);
        unsigned crossing = direct_setting("NCE_DIRECT_CROSS_PAGE", 0);
        unsigned epochs = direct_setting("NCE_DIRECT_ROUTE_EPOCHS", 0);
        unsigned retain = direct_setting("NCE_DIRECT_RETAIN_ROUTES", 0);
        unsigned groups = direct_setting("NCE_DIRECT_MEMORY_GROUPS", 0);
        unsigned native = direct_setting("NCE_DIRECT_NATIVE_EXCLUSIVE", 0);
        unsigned tables = direct_setting("NCE_DIRECT_VECTOR_TABLE", 0);
        unsigned math = direct_setting("NCE_DIRECT_SIMD_MATH", 0);
        REQUIRE(tables <= 1 && math <= 1);
        REQUIRE(watch <= 1 && crossing <= 1 && epochs <= 1 && retain <= 1 && groups <= 1 && native <= 1);
        REQUIRE(!retain || (watch && epochs));
        direct_code_watch = watch;
        const char *direct_memory = getenv("NCE_DIRECT_MEMORY");
        REQUIRE(!direct_memory || !strcmp(direct_memory, "cached") || !strcmp(direct_memory, "pte") || !strcmp(direct_memory, "refill") || !strcmp(direct_memory, "flat") || !strcmp(direct_memory, "view"));
        direct_pte_memory = direct_memory && strcmp(direct_memory, "cached");
        direct_pte_refill = direct_memory && !strcmp(direct_memory, "refill");
        direct_read_views = direct_memory && !strcmp(direct_memory, "view");
        direct_flat = direct_read_views || (direct_memory && !strcmp(direct_memory, "flat"));
        translation->flat_enabled = direct_flat;
        if (direct_read_views && cpu_index == 0) {
            REQUIRE(!nce_direct_install_read_faults());
            translation->read_view_create = read_view_create;
            translation->read_view_update = read_view_update;
            translation->read_view_destroy = read_view_destroy;
        }
        REQUIRE(selective <= 1 && reuse <= 1);
        NceDirectConfig direct_config = {cache_bytes, direct_setting("NCE_DIRECT_MAPPING_ENTRIES", 256),
                                         direct_setting("NCE_DIRECT_ROUTE_ENTRIES", 256), selective != 0, reuse != 0, crossing != 0, epochs != 0, retain != 0, groups != 0, native != 0, tables != 0, math != 0};
        REQUIRE(!direct_pte_memory || direct_pte_refill || (direct_config.mapping_entries == 256 && !selective));
        execution->direct_engine = nce_direct_create(direct_config);
        if (!execution->direct_engine)
            goto failed;
#ifdef NCE_SMP
        REQUIRE(direct_flat && !direct_read_views && native && !retain);
        nce_direct_set_code_guard(execution->direct_engine, (const uint64_t *)translation->code_guard, NCE_RAM_SIZE / PAGE);
#endif
        printf("{\"cpu\":%u,\"config\":\"translation_execution\",\"engine\":\"direct\",\"direct_cache_bytes\":%lu,\"data_page_bytes\":16384,\"mapping_entries\":%u,\"route_entries\":%u,\"selective_writes\":%s,\"reuse_code_checks\":%s,\"memory\":\"%s\",\"code_watch\":%s,\"cross_page\":%s,\"route_epochs\":%s,\"retain_routes\":%s,\"memory_groups\":%s,\"native_exclusive\":%s,\"vector_table\":%s,\"simd_math\":%s}\n",
               execution->index, cache_bytes, direct_config.mapping_entries, direct_config.route_entries, direct_config.selective_writes ? "true" : "false", direct_config.reuse_code_checks ? "true" : "false", direct_memory ? direct_memory : "cached", direct_code_watch ? "true" : "false", crossing ? "true" : "false", epochs ? "true" : "false", retain ? "true" : "false", groups ? "true" : "false", native ? "true" : "false", tables ? "true" : "false", math ? "true" : "false");
        if (direct_code_watch && cpu_index == 0) {
            original_page_free = nce_lkl_host.page_free;
            nce_lkl_host.page_free = watch_page_free;
        }
        if ((direct_code_watch || execution_count > 1) && cpu_index == 0) {
            original_disk_io = nce_lkl_host.disk_io;
            nce_lkl_host.disk_io = watch_disk_io;
        }
#else
        printf("{\"cpu\":%u,\"config\":\"translation_execution\",\"engine\":\"unicorn\"}\n", execution->index);
#endif
        // TLB mode initializes Unicorn; buffer size must be supplied before that call.
        if (uc_ctl_set_tcg_buffer_size(execution->engine, (uint32_t)cache_bytes) != UC_ERR_OK)
            goto failed;
#ifdef NCE_TRANSLATION_DUAL_MAP
        if (nce_uc_code_reserve(cache_bytes))
            goto failed;
#endif
        if (uc_ctl_tlb_mode(execution->engine, UC_TLB_VIRTUAL) != UC_ERR_OK)
            goto failed;
#ifdef NCE_TRANSLATION_DUAL_MAP
        nce_uc_code_cancel_reservation();
#endif
        const char *fastpath = getenv("NCE_TRANSLATION_LINUX_FASTPATH");
        REQUIRE(!fastpath || !strcmp(fastpath, "0") || !strcmp(fastpath, "1"));
        checked(nce_uc_set_linux_memory(execution->engine, fastpath && !strcmp(fastpath, "1")));
        uint64_t stats[5];
        nce_uc_cache_stats_aarch64(execution->engine, stats);
        REQUIRE(stats[4] <= cache_bytes && cache_bytes - stats[4] < 65536);
        printf("{\"cpu\":%u,\"config\":\"translation_cache_settings\",\"cache_bytes\":%lu,\"actual_cache_bytes\":%llu,\"invalidation\":\"%s\",\"linux_fastpath\":%s}\n",
               execution->index, cache_bytes, stats[4], page_invalidation ? "pages" : "full",
               fastpath && !strcmp(fastpath, "1") ? "true" : "false");
#ifdef NCE_DIRECT_EXECUTION
        uc_arm64_cp_reg dczid = {.op0 = 3, .op1 = 3, .crn = 0, .crm = 0, .op2 = 7};
        if (uc_reg_read(execution->engine, UC_ARM64_REG_CP_REG, &dczid) != UC_ERR_OK)
            goto failed;
        REQUIRE(dczid.val == 4);
#endif
        if (uc_context_alloc(execution->engine, &execution->empty_cpu) != UC_ERR_OK)
            goto failed;
        if (uc_context_save(execution->engine, execution->empty_cpu) != UC_ERR_OK)
            goto failed;
        uc_hook hook;
        if (uc_hook_add(execution->engine, &hook, UC_HOOK_TLB_FILL, fill, NULL, 1, 0) != UC_ERR_OK)
            goto failed;
        if (uc_hook_add(execution->engine, &hook, UC_HOOK_INTR, interrupt, NULL, 1, 0) != UC_ERR_OK)
            goto failed;
        if (translation && uc_hook_add(execution->engine, &hook, UC_HOOK_INSN, cache_instruction, NULL, 1, 0, UC_ARM64_INS_SYS) != UC_ERR_OK)
            goto failed;
#ifdef NCE_SMP
        if (uc_hook_add(execution->engine, &hook, UC_HOOK_MEM_WRITE, fallback_write, NULL, 1, 0) != UC_ERR_OK)
            goto failed;
#endif
    }
    if (timer_mode) {
        if (pthread_create(&timer_thread, NULL, preempt_timer, NULL))
            goto failed;
        timer_started = true;
    }
    nce_lkl_host.shmem_init = shmem_init;
    nce_lkl_host.shmem_reclaim = shmem_reclaim;
    nce_lkl_host.shmem_mmap = shmem_map;
    nce_lkl_host.munmap = revoke_pages;
    nce_lkl_host.user_clone = user_clone;
    nce_lkl_host.user_free = user_free;
    nce_lkl_host.user_switch = user_switch;
    nce_lkl_host.user_run = user_run;
    nce_lkl_host.user_exec = user_exec;
    translation_elf_initialize(guest_base);
    nce_lkl_host.user_elf = translation_elf_layout;
    nce_lkl_host.user_elf_offset = translation_elf_offset;
    nce_lkl_host.user_reserved = user_reserved;
    return guest_base;
failed:
    puts("{\"result\":\"failed\",\"reason\":\"translation_setup\"}");
    translation_linux_abort_setup();
    return 0;
}

void *translation_linux_process(const char *path)
{
    Process *p = calloc(1, sizeof(*p));
    REQUIRE(p);
    p->path = path;
    __atomic_fetch_add(&allocated, 1, __ATOMIC_RELAXED);
    return p;
}

void translation_linux_cleanup(void)
{
    if (timer_started) {
        REQUIRE(pthread_mutex_lock(&timer_lock) == 0);
        for (unsigned i = 0; i < execution_count; ++i)
            REQUIRE(!executions[i].timer_active);
        timer_stopping = true;
        REQUIRE(pthread_cond_signal(&timer_condition) == 0);
        REQUIRE(pthread_mutex_unlock(&timer_lock) == 0);
        REQUIRE(pthread_join(timer_thread, NULL) == 0);
        timer_started = false;
    }
    if (parallel_profile) {
        uint64_t total = 0;
        for (unsigned i = 0; i < execution_count; ++i)
            total += executions[i].run_cpu_ns;
        printf("{\"parallel_execution_profile\":true,\"cpus\":%u,\"union_wall_ns\":%llu,\"overlap_ns\":%llu,\"thread_cpu_ns\":%llu,\"cpu0_ns\":%llu,\"cpu1_ns\":%llu}\n",
               execution_count, (unsigned long long)profile_union_ns, (unsigned long long)profile_overlap_ns,
               (unsigned long long)total, (unsigned long long)executions[0].run_cpu_ns,
               (unsigned long long)executions[1].run_cpu_ns);
    }
    unsigned long fills = 0, faults = 0, calls = 0, preemptions = 0;
    for (unsigned i = 0; i < execution_count; ++i) {
        fills += executions[i].fills;
        faults += executions[i].faults;
        calls += executions[i].calls;
        preemptions += executions[i].preemptions;
    }
    printf("{\"test\":\"translated_context_cleanup\",\"passed\":%s,\"allocated\":%lu,\"freed\":%lu,\"maps\":%lu,\"revocations\":%lu,\"tlb_fills\":%lu,\"faults\":%lu,\"syscalls\":%lu,\"preemptions\":%lu}\n",
           allocated == freed ? "true" : "false", allocated, freed, maps, revocations, fills, faults, calls, preemptions);
    for (unsigned i = 0; i < execution_count; ++i) {
        ExecutionCpu *execution = &executions[i];
        printf("{\"cpu\":%u,\"translation_cache\":\"%s\",\"full_flushes\":%lu,\"invalidated_pages\":%lu,\"tlb_flushes\":%lu}\n", execution->index,
               page_invalidation ? "pages" : "full", execution->cache_flushes, execution->cache_pages, execution->tlb_flushes);
        printf("{\"cpu\":%u,\"translation_icache\":true,\"operations\":%lu,\"invalidations\":%lu}\n",
               execution->index, execution->icache_operations, execution->icache_invalidations);
        printf("{\"cpu\":%u,\"translation_tlb\":\"%s\",\"full_flushes\":%lu,\"invalidated_pages\":%lu,\"inactive_skips\":%lu,\"mm_switches\":%lu,\"inactive_mapping_changes\":%lu}\n", execution->index,
               page_tlb_invalidation ? "pages" : "full", execution->tlb_flushes, execution->tlb_pages, execution->tlb_inactive_skips,
               translation ? execution->view->mm_switches : 0, translation ? execution->view->inactive_mapping_changes : 0);
        uint64_t stats[5];
        nce_uc_cache_stats_aarch64(execution->engine, stats);
        printf("{\"cpu\":%u,\"translation_engine_cache\":true,\"total_flushes\":%llu,\"invalidated_blocks\":%llu,\"current_blocks\":%llu,\"used_bytes\":%llu,\"capacity_bytes\":%llu}\n", execution->index,
               stats[0], stats[1], stats[2], stats[3], stats[4]);
#ifdef NCE_DIRECT_EXECUTION
        REQUIRE(!code_watch);
        printf("{\"cpu\":%u,\"direct_code_watch\":true,\"scope\":\"process\",\"enabled\":%s,\"queries\":%llu,\"protections\":%llu,\"write_faults\":%llu,\"invalidations\":%llu,\"protection_failures\":%llu,\"excluded_pages\":%llu}\n", execution->index,
               direct_code_watch ? "true" : "false", code_watch_stats.queries, code_watch_stats.protections,
               code_watch_stats.write_faults, code_watch_stats.invalidations, code_watch_stats.protection_failures, code_watch_stats.excluded_pages);
        printf("{\"cpu\":%u,\"direct_flat_tables\":true,\"enabled\":%s,\"live_virtual_bytes\":%lu,\"peak_virtual_bytes\":%lu,\"updates\":%lu,\"refresh_pages\":%lu}\n", execution->index,
               direct_flat ? "true" : "false", translation->flat_live_bytes, translation->flat_peak_bytes,
               translation->flat_updates, translation->flat_refresh_pages);
        REQUIRE(!translation->flat_live_bytes);
        NceDirectStats direct = nce_direct_stats(execution->direct_engine);
        REQUIRE(!read_views_live && read_views_created == read_views_destroyed);
        printf("{\"cpu\":%u,\"direct_read_views\":true,\"enabled\":%s,\"created\":%lu,\"destroyed\":%lu,\"live_virtual_bytes\":%lu,\"peak_virtual_bytes\":%lu,\"maps\":%lu,\"revocations\":%lu,\"read_changes\":%lu,\"write_changes\":%lu,\"compiled_reads\":%llu,\"read_faults\":%llu}\n", execution->index,
               direct_read_views ? "true" : "false", read_views_created, read_views_destroyed,
               read_views_live, read_views_peak, read_view_maps, read_view_revocations,
               translation->flat_read_changes, translation->flat_write_changes, direct.read_view_sites, direct.read_view_faults);
        printf("{\"cpu\":%u,\"direct_memory_groups\":true,\"groups\":%llu,\"instructions\":%llu}\n", execution->index,
               direct.memory_groups, direct.memory_group_instructions);
        printf("{\"cpu\":%u,\"direct_code_routes\":true,\"invalidations\":%llu,\"cleared_entries\":%llu,\"branch_exits\":%llu,\"reused_runs\":%llu}\n", execution->index,
               direct.route_invalidations, direct.route_cleared_entries, direct.branch_exits, direct.route_reused_runs);
        printf("{\"cpu\":%u,\"direct_code_validation\":true,\"content_checks\":%llu,\"version_hits\":%llu}\n", execution->index, direct.code_content_checks, direct.code_version_hits);
        printf("{\"cpu\":%u,\"direct_fallback_reasons\":true,\"alignment\":%llu,\"cross_page\":%llu,\"code_write\":%lu}\n", execution->index,
               direct.alignment_exits, direct.cross_page_fallbacks, execution->direct_code_writes);
        printf("{\"cpu\":%u,\"direct_cross_page\":true,\"exits\":%llu,\"completed\":%llu,\"fallbacks\":%llu,\"faults\":%llu,\"bytes\":%llu}\n", execution->index,
               direct.cross_page_exits, direct.cross_page_completed, direct.cross_page_fallbacks, direct.cross_page_faults, direct.cross_page_bytes);
        printf("{\"cpu\":%u,\"direct_execution\":true,\"calls\":%lu,\"fallbacks\":%lu,\"compiles\":%llu,\"flushes\":%llu,\"code_changes\":%llu,\"code_bytes\":%zu,\"capacity_bytes\":%zu,\"pages\":%zu,\"mapping_misses\":%llu,\"code_lookups\":%llu,\"read_mapping_misses\":%llu,\"write_mapping_misses\":%llu,\"write_invalidations\":%llu,\"code_validation_hits\":%llu,\"pte_read_refills\":%llu}\n", execution->index,
               execution->direct_calls, execution->direct_fallbacks, direct.compiles, direct.flushes, direct.code_changes,
               direct.code_bytes, direct.code_capacity, direct.pages, direct.mapping_misses, direct.code_lookups,
               direct.read_mapping_misses, direct.write_mapping_misses, direct.write_invalidations, direct.code_validation_hits, direct.pte_read_refills);
        for (unsigned i = 0; i < 256; ++i)
            if (execution->direct_missing[i].count)
                printf("{\"cpu\":%u,\"direct_fallback\":%u,\"count\":%lu}\n", execution->index, execution->direct_missing[i].instruction, execution->direct_missing[i].count);
        nce_direct_destroy(execution->direct_engine);
        if (direct_read_views)
            REQUIRE(!nce_direct_remove_read_faults());
#endif
        REQUIRE(allocated == freed);
        checked(uc_context_free(execution->empty_cpu));
        checked(uc_close(execution->engine));
    }
    REQUIRE(munmap(ram, NCE_RAM_SIZE) == 0);
    REQUIRE(munmap((void *)reservation_base, reservation_size) == 0);
    free(pages);
    if (translation)
        free(translation->code_guard);
    translation_elf_cleanup();
}

/* Before kernel init there are no guest contexts, RAM or ELF registrations. */
void translation_linux_abort_setup(void)
{
    if (timer_started) {
        REQUIRE(!pthread_mutex_lock(&timer_lock));
        timer_stopping = true;
        REQUIRE(!pthread_cond_signal(&timer_condition));
        REQUIRE(!pthread_mutex_unlock(&timer_lock));
        REQUIRE(!pthread_join(timer_thread, NULL));
        timer_started = false;
    }
#ifdef NCE_TRANSLATION_DUAL_MAP
    nce_uc_code_cancel_reservation();
#endif
    for (unsigned i = 0; i < execution_count; ++i) {
        ExecutionCpu *execution = &executions[i];
        if (execution->empty_cpu)
            checked(uc_context_free(execution->empty_cpu));
        if (execution->engine)
            checked(uc_close(execution->engine));
#ifdef NCE_DIRECT_EXECUTION
        nce_direct_destroy(execution->direct_engine);
#endif
        *execution = (ExecutionCpu){0};
    }
#ifdef NCE_DIRECT_EXECUTION
    if (direct_read_views)
        REQUIRE(!nce_direct_remove_read_faults());
    if (original_page_free)
        nce_lkl_host.page_free = original_page_free;
    if (original_disk_io)
        nce_lkl_host.disk_io = original_disk_io;
#endif
    if (reservation_base)
        REQUIRE(!munmap((void *)reservation_base, reservation_size));
    guest_base = guest_end = physical_base = reservation_base = reservation_size = 0;
    free(pages);
    pages = NULL;
    if (translation) {
        free(translation->code_guard);
        translation->code_guard = NULL;
    }
}

#ifdef NCE_TRANSLATION_DUAL_MAP
int translation_linux_check_code(void)
{
#ifdef NCE_DIRECT_EXECUTION
    for (unsigned i = 0; i < execution_count; ++i)
        if (nce_direct_check_code(executions[i].direct_engine))
            return -1;
#endif
    return nce_uc_code_check_all();
}
#endif
