/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "art-aot-targets.h"
#include "nce.h"
#include "memory.h"
#include "layout.h"
#include "lkl-mmu-profile.h"
#include "lkl-host-ops.h"
#include "guest-image.h"
#include "programs.h"
#ifdef NCE_GRAPHICS
#include "graphics/host-graphics.h"
#endif
#include "input.h"
#include "session.h"
#include "user-scripts.h"
#include <mach/mach.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/resource.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <time.h>

#define REQUIRE(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "N2 failure %s:%d: %s\n", __FILE__, __LINE__, #c); \
            abort(); \
        } \
    } while (0)
static uintptr_t user_base;
#define USER_BASE user_base
#define USER_SIZE NCE_USER_SIZE
#define PAGE NCE_PAGE_SIZE
extern struct lkl_host_operations nce_lkl_host;
typedef struct {
    NceState state;
    void *signal_stack;
    const char *exec_path;
    pthread_t thread;
    uintptr_t fault_address;
    uint32_t fault_instruction;
    int event_kind, fault_write, fault_illegal, fault_execute, stack_unmapped;
} Process;
static _Thread_local Process *this_process;
static pthread_mutex_t target_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t target_thread, timer_thread;
static atomic_int timer_stopping;
static NceCode event_resume;
static atomic_ulong resume_hits, resume_misses;
static unsigned long preemptions, deferred_faults, stack_resumes, stack_calls, stackless_exits;
static unsigned long inherited_preempt_blocks;
static volatile sig_atomic_t logged_redirect, logged_stack_restore;
static struct sigaction old_preempt;

static NceState *active;
static NceCode code;
static unsigned char *ram;
static unsigned long code_copies, code_copy_bytes, data_maps, data_map_bytes, revocations;
static unsigned long code_reused_bytes;
static bool map_profile;
static bool direct_fault;
static struct nce_mmu_profile mmu_profile;

struct nce_mmu_profile *nce_process_mmu_profile(void)
{
    return map_profile ? &mmu_profile : NULL;
}
enum MapOperation { MAP_DATA,
                    MAP_CODE,
                    MAP_REVOKE,
                    MAP_OPERATIONS };
static atomic_ulong map_calls[MAP_OPERATIONS], map_time_ns[MAP_OPERATIONS];

static uint64_t profile_clock(void)
{
    return map_profile ? clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) : 0;
}

static void profile_mapping(enum MapOperation operation, uint64_t begin)
{
    if (!map_profile)
        return;
    atomic_fetch_add_explicit(&map_calls[operation], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&map_time_ns[operation], profile_clock() - begin, memory_order_relaxed);
}

static NceSegment dynamic_cache;
static size_t ram_size;
static unsigned long reclaimed_ranges, reclaimed_bytes;
static long (*run_syscall)(long, long *);
static int (*page_fault)(unsigned long, int);
static void (*user_return)(long, long, unsigned long);
static void (*user_fault)(unsigned long, int);
static void (*user_preempt)(void);
static void (*prepare_user)(void);
static unsigned long faults, allocated, freed;
static int trace, fault_trace;
static struct sigaction old_segv, old_bus, old_ill, old_trap;

static void shmem_init(unsigned long size)
{
    ram_size = size;
    REQUIRE(size == NCE_RAM_SIZE);
    ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(ram != MAP_FAILED);
}
static int shmem_reclaim(unsigned long offset, unsigned long size)
{
    REQUIRE(!(offset % PAGE) && !(size % PAGE) && offset <= ram_size && size <= ram_size - offset);
    /* Anonymous memory has both the RAM backing and the Linux direct-map alias.
     * Replace both views while Linux keeps the free pages isolated. */
    REQUIRE(mmap(ram + offset, size, PROT_READ | PROT_WRITE,
                 MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == ram + offset);
    REQUIRE(nce_memory_remap((void *)(USER_BASE + USER_SIZE + offset), ram + offset,
                             size, PROT_READ | PROT_WRITE) == 0);
    ++reclaimed_ranges;
    reclaimed_bytes += size;
    return 0;
}
static void restore_code(NceProgram *program, NceSegment *segment, size_t position,
                         const unsigned char *source, size_t length, enum lkl_prot prot)
{
    uint64_t begin = profile_clock();
    /* Darwin's cache operation needs an accessible RX address. No guest
     * executes concurrently while the single Linux CPU restores this view. */
    REQUIRE(mprotect(segment->code.rx + position, length,
                     PROT_READ | (prot & LKL_PROT_EXEC ? PROT_EXEC : 0)) == 0);
    if (!segment->source_cache) {
        segment->source_cache = mmap(NULL, segment->size, PROT_READ | PROT_WRITE,
                                     MAP_ANON | MAP_PRIVATE, -1, 0);
        segment->cache_prot = calloc(segment->size / PAGE, 1);
        REQUIRE(segment->source_cache != MAP_FAILED && segment->cache_prot);
    }
    unsigned char mode = prot & LKL_PROT_EXEC ? 2 : 1;
    for (size_t offset = position; offset < position + length; offset += PAGE) {
        size_t index = offset / PAGE;
        const unsigned char *page = source + offset - position;
        /* Compare original bytes as well as permissions. Linux can reuse a
         * physical page or change a JIT backing file without changing its PTE. */
        if (segment->cache_prot[index] == mode &&
            !memcmp(segment->source_cache + offset, page, PAGE)) {
            code_reused_bytes += PAGE;
            continue;
        }
        memcpy(segment->source_cache + offset, page, PAGE);
        memcpy(segment->code.rw + offset, page, PAGE);
        if (program && mode == 2)
            REQUIRE(nce_program_patch_range(program, segment, offset, PAGE) == 0);
        nce_sync_code((NceCode){segment->code.rw + offset, segment->code.rx + offset, PAGE});
        segment->cache_prot[index] = mode;
        ++code_copies;
        code_copy_bytes += PAGE;
    }
    profile_mapping(MAP_CODE, begin);
}

static void *shmem_map(void *address, unsigned long offset, unsigned long size, enum lkl_prot prot)
{
    uintptr_t start = (uintptr_t)address;
    REQUIRE(!(offset % PAGE) && !(size % PAGE) && offset <= ram_size && size <= ram_size - offset);
    uintptr_t end = start + size;
    REQUIRE(!nce_program_reserved(start, size));
    while (start < end) {
        uintptr_t stop = end;
        NceProgram *program = NULL;
        NceSegment *segment = NULL;
        for (size_t i = nce_program_index(start); i < nce_program_count && nce_programs[i].base < stop; ++i) {
            NceProgram *p = &nce_programs[i];
            for (size_t j = 0; j < p->segment_count; ++j) {
                NceSegment *s = &p->segments[j];
                uintptr_t lo = (uintptr_t)s->code.rx, hi = lo + s->size;
                if (start >= lo && start < hi) {
                    program = p;
                    segment = s;
                    stop = hi < stop ? hi : stop;
                } else if (lo > start && lo < stop)
                    stop = lo;
            }
        }
        NceCode dynamic = nce_memory_dynamic_code();
        uintptr_t dynamic_start = (uintptr_t)dynamic.rx;
        int in_dynamic = start >= dynamic_start && start < dynamic_start + dynamic.size;
        if (in_dynamic && stop > dynamic_start + dynamic.size)
            stop = dynamic_start + dynamic.size;
        else if (start < dynamic_start && stop > dynamic_start)
            stop = dynamic_start;
        size_t length = stop - start;
        if (segment) {
            size_t position = start - (uintptr_t)segment->code.rx;
            if (nce_memory_code_protection()) {
                restore_code(program, segment, position, ram + offset, length, prot);
                start = stop;
                offset += length;
                continue;
            }
            memcpy(segment->code.rw + position, ram + offset, length);
            if (prot & LKL_PROT_EXEC)
                REQUIRE(nce_program_patch_range(program, segment, position, length) == 0);
            nce_sync_code((NceCode){segment->code.rw + position, (void *)start, length});
            ++code_copies;
            code_copy_bytes += length;
        } else if (in_dynamic) {
            REQUIRE(!(prot & LKL_PROT_WRITE));
            if (nce_memory_code_protection()) {
                restore_code(NULL, &dynamic_cache, start - dynamic_start, ram + offset, length, prot);
                start = stop;
                offset += length;
                continue;
            }
            unsigned char *target = dynamic.rw + start - dynamic_start;
            if (prot & LKL_PROT_EXEC)
                memcpy(target, ram + offset, length);
            else
                memset(target, 0, length);
            nce_sync_code((NceCode){target, (void *)start, length});
            ++code_copies;
            code_copy_bytes += length;
        } else {
            REQUIRE(!(prot & LKL_PROT_EXEC));
            int host_prot = ((prot & LKL_PROT_READ) ? PROT_READ : 0) | ((prot & LKL_PROT_WRITE) ? PROT_WRITE : 0);
            uint64_t begin = profile_clock();
            REQUIRE(nce_memory_remap((void *)start, ram + offset, length, host_prot) == 0);
            profile_mapping(MAP_DATA, begin);
            ++data_maps;
            data_map_bytes += length;
        }
        start = stop;
        offset += length;
    }
    return address;
}
static int protect_none(void *address, unsigned long size)
{
    uint64_t begin = profile_clock();
    ++revocations;
    uintptr_t start = (uintptr_t)address, end = start + size;
    uintptr_t cursor = start;
    for (size_t i = nce_program_index(start); i < nce_program_count && nce_programs[i].base < end; ++i) {
        NceProgram *p = &nce_programs[i];
        for (size_t j = 0; j < p->segment_count; ++j) {
            NceSegment *s = &p->segments[j];
            uintptr_t lo = (uintptr_t)s->code.rx, hi = lo + s->size;
            if (hi <= start || lo >= end)
                continue;
            if (cursor < lo && mmap((void *)cursor, lo - cursor, PROT_NONE,
                                    MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED)
                return -1;
            size_t first = start > lo ? start - lo : 0;
            size_t stop = end < hi ? end - lo : s->size;
            if (nce_memory_code_protection()) {
                if (mprotect(s->code.rx + first, stop - first, PROT_NONE))
                    return -1;
            } else {
                memset(s->code.rw + first, 0, stop - first);
                nce_sync_code((NceCode){s->code.rw + first, s->code.rx + first, stop - first});
            }
            cursor = lo + stop;
        }
        /* Branch stubs belong to the host and survive guest address-space changes. */
        uintptr_t lo = (uintptr_t)p->stubs.rx, hi = lo + p->stubs.size;
        if (hi > start && lo < end) {
            if (cursor < lo && mmap((void *)cursor, lo - cursor, PROT_NONE,
                                    MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED)
                return -1;
            cursor = hi < end ? hi : end;
        }
    }
    NceCode dynamic = nce_memory_dynamic_code();
    uintptr_t lo = (uintptr_t)dynamic.rx, hi = lo + dynamic.size;
    if (hi > start && lo < end) {
        if (cursor < lo && mmap((void *)cursor, lo - cursor, PROT_NONE,
                                MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED)
            return -1;
        size_t first = start > lo ? start - lo : 0;
        size_t stop = end < hi ? end - lo : dynamic.size;
        /* The first dynamic page contains the host's resume branch. */
        if (first < PAGE)
            first = PAGE;
        if (stop < first)
            stop = first;
        if (nce_memory_code_protection()) {
            if (stop > first && mprotect(dynamic.rx + first, stop - first, PROT_NONE))
                return -1;
        } else {
            memset(dynamic.rw + first, 0, stop - first);
            nce_sync_code((NceCode){dynamic.rw + first, dynamic.rx + first, stop - first});
        }
        cursor = lo + stop;
    }
    if (cursor < end && mmap((void *)cursor, end - cursor, PROT_NONE,
                             MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED)
        return -1;
    profile_mapping(MAP_REVOKE, begin);
    return 0;
}

static void capture_state(NceState *s, ucontext_t *u)
{
    memcpy(s->regs.x, u->uc_mcontext->__ss.__x, 18 * sizeof(uint64_t));
    memcpy(s->regs.x + 19, u->uc_mcontext->__ss.__x + 19, 10 * sizeof(uint64_t));
    s->regs.x[29] = __darwin_arm_thread_state64_get_fp(u->uc_mcontext->__ss);
    s->regs.x[30] = __darwin_arm_thread_state64_get_lr(u->uc_mcontext->__ss);
    s->regs.sp = __darwin_arm_thread_state64_get_sp(u->uc_mcontext->__ss);
    s->regs.pc = __darwin_arm_thread_state64_get_pc(u->uc_mcontext->__ss);
    s->regs.nzcv = u->uc_mcontext->__ss.__cpsr & 0xf0000000u;
    s->regs.fpcr = u->uc_mcontext->__ns.__fpcr;
    s->regs.fpsr = u->uc_mcontext->__ns.__fpsr;
    memcpy(s->regs.vector, u->uc_mcontext->__ns.__v, sizeof(s->regs.vector));
}

static void restore_signal_state(NceState *s, ucontext_t *u)
{
    memcpy(u->uc_mcontext->__ss.__x, s->regs.x, 29 * sizeof(uint64_t));
    u->uc_mcontext->__ss.__x[18] = s->host_x18;
    __darwin_arm_thread_state64_set_fp(u->uc_mcontext->__ss, s->regs.x[29]);
    __darwin_arm_thread_state64_set_lr_fptr(u->uc_mcontext->__ss, (void (*)(void))s->regs.x[30]);
    __darwin_arm_thread_state64_set_sp(u->uc_mcontext->__ss, s->regs.sp);
    __darwin_arm_thread_state64_set_pc_fptr(u->uc_mcontext->__ss, (void (*)(void))s->regs.pc);
    u->uc_mcontext->__ss.__cpsr = (u->uc_mcontext->__ss.__cpsr & ~0xf0000000u) | (uint32_t)s->regs.nzcv;
    u->uc_mcontext->__ns.__fpcr = (uint32_t)s->regs.fpcr;
    u->uc_mcontext->__ns.__fpsr = (uint32_t)s->regs.fpsr;
    memcpy(u->uc_mcontext->__ns.__v, s->regs.vector, sizeof(s->regs.vector));
}

static int guest_pc(uintptr_t pc)
{
    NceCode dynamic = nce_memory_dynamic_code();
    if (pc >= (uintptr_t)dynamic.rx + PAGE && pc < (uintptr_t)dynamic.rx + dynamic.size)
        return 1;
    if (pc >= (uintptr_t)code.rx && pc < (uintptr_t)code.rx + sizeof(guest_image))
        return 1;
    size_t i = nce_program_index(pc);
    if (i < nce_program_count)
        for (size_t j = 0; j < nce_programs[i].segment_count; ++j) {
            NceCode c = nce_programs[i].segments[j].code;
            if (pc >= (uintptr_t)c.rx && pc < (uintptr_t)c.rx + c.size)
                return 1;
        }
    return 0;
}

static int syscall_stub(uintptr_t pc)
{
    uintptr_t fixture = (uintptr_t)code.rx + ((sizeof(guest_image) + 15) & ~(size_t)15) + 16;
    if (code.rx && pc >= fixture && pc + 80 <= (uintptr_t)code.rx + code.size && (pc - fixture) % 80 == 0) {
        uintptr_t gate;
        memcpy(&gate, (void *)(pc + 72), sizeof(gate));
        if (gate == (uintptr_t)nce_gate)
            return 1;
    }
    size_t i = nce_program_index(pc);
    if (i < nce_program_count) {
        NceProgram *p = &nce_programs[i];
        uintptr_t begin = (uintptr_t)p->stubs.rx + 16;
        if (p->stubs.rx && pc >= begin && pc < begin + p->patch_count * 80 && !((pc - begin) % 80))
            return 1;
    }
    return 0;
}

static void prepare_resume(NceState *state)
{
    NceCode resume = event_resume;
    NceCode dynamic = nce_memory_dynamic_code();
    if (state->regs.pc >= (uintptr_t)dynamic.rx && state->regs.pc < (uintptr_t)dynamic.rx + dynamic.size)
        resume = (NceCode){dynamic.rw, dynamic.rx, PAGE};
    size_t i = nce_program_index(state->regs.pc);
    if (i < nce_program_count) {
        NceProgram *p = &nce_programs[i];
        if (state->regs.pc >= p->base && state->regs.pc < p->base + p->span) {
            size_t offset = 16 + p->patch_count * 80;
            resume = (NceCode){p->stubs.rw + offset, p->stubs.rx + offset, p->stubs.size - offset};
        }
    }
    // The Linux CPU owns these slots until the guest resumes. Their branch word
    // identifies the target; a collision only replaces this slot's old entry.
    REQUIRE(resume.size >= 16);
    size_t slot = (state->regs.pc >> 2) % (resume.size / 16) * 16;
    resume = (NceCode){resume.rw + slot, resume.rx + slot, 16};
    uint32_t branch;
    REQUIRE(nce_write_branch(&branch, (uintptr_t)resume.rx + 4, state->regs.pc) == 0);
    if (((uint32_t *)resume.rw)[1] != branch) {
        REQUIRE(nce_write_entry(resume, state->regs.pc) == 0);
        nce_sync_code(resume);
        if (map_profile)
            atomic_fetch_add_explicit(&resume_misses, 1, memory_order_relaxed);
    } else if (map_profile) {
        atomic_fetch_add_explicit(&resume_hits, 1, memory_order_relaxed);
    }
    state->resume = (uintptr_t)resume.rx;
}

static NceState *handle_event(NceState *state)
{
    Process *p = (void *)state;
    if (p->event_kind == 1) {
        if (!deferred_faults)
            puts("{\"phase\":\"guest_fault_on_host_stack\"}");
        int result = page_fault(p->fault_address, p->fault_execute ? NCE_FAULT_EXEC : p->fault_write);
        if (result && (trace || fault_trace || p->fault_illegal)) {
            fprintf(stderr, "guest fault pc=%lx address=%lx write=%d illegal=%d result=%d instruction=%08x trapped=%08x\n",
                    state->regs.pc, p->fault_address, p->fault_write, p->fault_illegal, result,
                    p->fault_illegal ? *(const uint32_t *)state->regs.pc : 0, p->fault_instruction);
            if (fault_trace)
                fprintf(stderr, "fault registers x0=%lx x1=%lx x8=%lx x19=%lx sp=%lx\n",
                        state->regs.x[0], state->regs.x[1], state->regs.x[8], state->regs.x[19], state->regs.sp);
            for (size_t i = 0; i < nce_program_count; ++i) {
                NceProgram *program = &nce_programs[i];
                if (state->regs.pc >= program->base && state->regs.pc - program->base < program->span)
                    fprintf(stderr, "guest program=%s offset=%lx\n", program->path, state->regs.pc - program->base);
            }
        }
        if (result)
            user_fault(p->fault_address, p->fault_illegal && result == -8);
        else
            user_return(-1, 0, 0);
        ++faults;
        ++deferred_faults;
    } else if (p->event_kind == 2) {
        user_preempt();
        ++preemptions;
    } else if (p->event_kind == 4) {
        if (fault_trace) {
            fprintf(stderr, "guest trap pc=%lx lr=%lx instruction=%08x x0=%lx x1=%lx x8=%lx\n",
                    state->regs.pc, state->regs.x[30], *(const uint32_t *)state->regs.pc,
                    state->regs.x[0], state->regs.x[1], state->regs.x[8]);
            for (size_t i = 0; i < nce_program_count; ++i) {
                NceProgram *program = &nce_programs[i];
                if (state->regs.pc >= program->base && state->regs.pc - program->base < program->span)
                    fprintf(stderr, "guest trap program=%s offset=%lx\n", program->path, state->regs.pc - program->base);
            }
        }
        user_fault(state->regs.pc, 2);
    } else {
        ++stack_calls;
        if (p->stack_unmapped && state->regs.x[8] == 93)
            ++stackless_exits;
        nce_dispatch(state);
    }
    prepare_resume(state);
    return state;
}

static void fault(int signal, siginfo_t *info, void *opaque)
{
    ucontext_t *u = opaque;
    uintptr_t pc = __darwin_arm_thread_state64_get_pc(u->uc_mcontext->__ss);
    uintptr_t address = (uintptr_t)info->si_addr;
    int is_write = (u->uc_mcontext->__es.__esr & (1u << 6)) != 0;
    Process *p = this_process;
    if (p && active == &p->state) {
        if (signal != SIGUSR2 && pc == (uintptr_t)nce_restore_stack) {
            /* The guest may legally unmap its own stack before SYS_exit.
             * Signal return can restore every register without stack scratch. */
            if (!logged_stack_restore) {
                static const char message[] = "{\"phase\":\"signal_restore_without_guest_stack\"}\n";
                write(STDOUT_FILENO, message, sizeof(message) - 1);
                logged_stack_restore = 1;
            }
            restore_signal_state(&p->state, u);
            ++stack_resumes;
            return;
        }
        int stub = signal != SIGUSR2 && syscall_stub(pc);
        if (guest_pc(pc) || stub) {
            unsigned exception_class = u->uc_mcontext->__es.__esr >> 26;
            bool execute = signal == SIGILL || exception_class == 0x20 || exception_class == 0x21;
            if (direct_fault && (signal == SIGSEGV || signal == SIGBUS) && !execute &&
                address >= USER_BASE && address < USER_BASE + USER_SIZE && !guest_pc(address) &&
                page_fault(address, NCE_FAULT_TRY_PRESENT | is_write))
                return;
            if (!logged_redirect) {
                static const char message[] = "{\"phase\":\"signal_redirect_to_host_entry\"}\n";
                write(STDOUT_FILENO, message, sizeof(message) - 1);
                logged_redirect = 1;
            }
            capture_state(&p->state, u);
            p->event_kind = signal == SIGUSR2 ? 2 : signal == SIGTRAP ? 4
                                                : stub                ? 3
                                                                      : 1;
            p->fault_address = address;
            p->fault_write = is_write;
            p->fault_illegal = signal == SIGILL;
            p->fault_execute = execute;
            p->fault_instruction = signal == SIGILL ? *(const uint32_t *)pc : 0;
            if (stub) {
                p->state.resume = pc + 40;
                memcpy(&p->state.instruction, (void *)(pc + 64), 4);
            }
            u->uc_mcontext->__ss.__x[0] = (uintptr_t)&p->state;
            u->uc_mcontext->__ss.__x[18] = p->state.host_x18;
            __darwin_arm_thread_state64_set_sp(u->uc_mcontext->__ss, p->state.host_sp);
            __darwin_arm_thread_state64_set_pc_fptr(u->uc_mcontext->__ss, nce_event_gate);
            return;
        }
    }
    if (signal == SIGUSR2)
        return;
    fprintf(stderr, "N2 fatal signal=%d address=%p pc=%lx esr=%x active=%p\n",
            signal, info->si_addr, pc, u->uc_mcontext->__es.__esr, (void *)active);
    _Exit(128 + signal);
}

static void user_switch(void *state)
{
    active = state;
    REQUIRE(pthread_mutex_lock(&target_mutex) == 0);
    target_thread = state ? ((Process *)state)->thread : NULL;
    REQUIRE(pthread_mutex_unlock(&target_mutex) == 0);
}

void nce_process_thread_exit(void)
{
    REQUIRE(pthread_mutex_lock(&target_mutex) == 0);
    if (target_thread == pthread_self())
        target_thread = NULL;
    REQUIRE(pthread_mutex_unlock(&target_mutex) == 0);
    this_process = NULL;
}

static void *preempt_timer(void *unused)
{
    (void)unused;
    while (!atomic_load(&timer_stopping)) {
        struct timespec interval = {.tv_nsec = 2000000};
        nanosleep(&interval, NULL);
        REQUIRE(pthread_mutex_lock(&target_mutex) == 0);
        if (target_thread)
            pthread_kill(target_thread, SIGUSR2);
        REQUIRE(pthread_mutex_unlock(&target_mutex) == 0);
    }
    return NULL;
}

static void *user_clone(void *parent, unsigned long sp, unsigned long tls, unsigned long flags)
{
    NceState *state = calloc(1, sizeof(Process));
    if (!state)
        return NULL;
    *state = *(NceState *)parent;
    state->regs.x[0] = 0;
    if (sp)
        state->regs.sp = sp;
    if (flags & 0x80000)
        state->tls = tls;
    ++allocated;
    return state;
}
static void user_free(void *state)
{
    Process *process = state;
    free(process->signal_stack);
    ++freed;
    free(process);
}
static int guest_memory(NceState *state, uintptr_t address, void *buffer, size_t size, int write)
{
    uintptr_t bad = address;
    if (address < USER_BASE || address >= USER_BASE + USER_SIZE || size > USER_BASE + USER_SIZE - address)
        goto fault;
    for (uintptr_t p = address; p < address + size; p = (p & ~(PAGE - 1)) + PAGE) {
        bad = p;
        if (page_fault(p, write | NCE_FAULT_DIRECT))
            goto fault;
    }
    prepare_user();
    if (write)
        memcpy((void *)address, buffer, size);
    else
        memcpy(buffer, (void *)address, size);
    return 0;
fault:
    if (fault_trace)
        fprintf(stderr, "operand fault pc=%lx address=%lx size=%zu write=%d x0=%lx x8=%lx\n",
                state->regs.pc - 4, bad, size, write, state->regs.x[0], state->regs.x[8]);
    state->regs.pc -= 4;
    user_fault(bad, 0);
    prepare_resume(state);
    return -1;
}
static void syscall_from_guest(NceState *state)
{
    long args[6];
    for (unsigned i = 0; i < 6; ++i)
        args[i] = state->regs.x[i];
    long number = state->regs.x[8];
    uintptr_t continuation = state->regs.pc;
    long result = run_syscall(number, args);
    if (trace)
        fprintf(stderr, "guest syscall=%ld pc=%lx args=%lx,%lx,%lx,%lx,%lx,%lx result=%lx\n",
                number, continuation, args[0], args[1], args[2], args[3], args[4], args[5], result);
    if (number == 215 && result == 0 && state->regs.sp >= (uintptr_t)args[0] &&
        state->regs.sp - (uintptr_t)args[0] < (uintptr_t)args[1])
        ((Process *)state)->stack_unmapped = 1;
    user_return(number, result, args[0]);
    if (state->regs.pc != continuation) {
        prepare_resume(state);
    }
}
static void user_run(void *opaque)
{
    NceState *state = opaque;
    Process *process = opaque;
    this_process = process;
    process->thread = pthread_self();
    state->event = handle_event;
    state->memory = guest_memory;
    user_switch(state);
    process->signal_stack = malloc(65536);
    REQUIRE(process->signal_stack);
    stack_t signal_stack = {.ss_sp = process->signal_stack, .ss_size = 65536};
    REQUIRE(sigaltstack(&signal_stack, NULL) == 0);
    /* pthreads created from the iOS dispatch worker inherit its blocked
     * SIGUSR2. Only the Linux-owned guest thread changes its signal mask. */
    sigset_t preempt_mask, inherited_mask;
    sigemptyset(&preempt_mask);
    sigaddset(&preempt_mask, SIGUSR2);
    REQUIRE(pthread_sigmask(SIG_UNBLOCK, &preempt_mask, &inherited_mask) == 0);
    if (sigismember(&inherited_mask, SIGUSR2)) {
        if (!inherited_preempt_blocks)
            puts("{\"phase\":\"unblocked_inherited_preemption_signal\"}");
        ++inherited_preempt_blocks;
    }
    if (!state->regs.sp) {
        long args[6] = {USER_BASE + USER_SIZE - 65536, 65536, 3, 0x32, -1, 0};
        REQUIRE(run_syscall(222, args) == args[0]);
        state->regs.sp = args[0] + args[1];
    }
    active = state;
    if (process->exec_path) {
        uintptr_t *stack = (void *)(state->regs.sp - 512);
        REQUIRE(page_fault((uintptr_t)stack, 1 | NCE_FAULT_DIRECT) == 0);
        prepare_user();
        char *path = (void *)(stack + 16);
        char *env = path + 64;
        strcpy(path, process->exec_path);
        strcpy(env, "NCE_TEST=userspace");
        stack[0] = (uintptr_t)path;
        stack[1] = 0;
        stack[2] = (uintptr_t)env;
        stack[3] = 0;
        long args[6] = {(long)path, (long)stack, (long)(stack + 2)};
        long result = run_syscall(221, args);
        if (result)
            fprintf(stderr, "exec %s failed: %ld\n", process->exec_path, result);
        REQUIRE(result == 0);
    }
    prepare_user();
    nce_enter(state);
    REQUIRE(0); /* Linux exit terminates the task without returning to NCE. */
}
static void user_exec(void *opaque, unsigned long entry, unsigned long stack)
{
    Process *process = opaque;
    NceState *state = &process->state;
    NceProgram *program = nce_program_entry(entry);
    REQUIRE(program);
    memset(state->regs.x, 0, sizeof(state->regs.x));
    memset(state->regs.vector, 0, sizeof(state->regs.vector));
    state->regs.sp = stack;
    state->tls = state->regs.nzcv = state->regs.fpcr = state->regs.fpsr = 0;
    state->regs.pc = entry;
    state->resume = (uintptr_t)program->stubs.rx;
}

unsigned long nce_process_setup(void)
{
    REQUIRE(getpagesize() == PAGE);
    trace = getenv("NCE_TRACE") != NULL;
    fault_trace = getenv("NCE_FAULT_TRACE") != NULL;
    const char *profile_mode = getenv("NCE_MAP_PROFILE");
    map_profile = profile_mode && strcmp(profile_mode, "0");
    const char *fault_mode = getenv("NCE_DIRECT_FAULT");
    direct_fault = !fault_mode || strcmp(fault_mode, "0");
    printf("{\"phase\":\"present_page_signal_restore\",\"enabled\":%s}\n", direct_fault ? "true" : "false");
    user_base = nce_memory_user_base();
    dynamic_cache = (NceSegment){.size = NCE_EXEC_SIZE, .code = nce_memory_dynamic_code()};
    for (size_t i = 0; i < nce_program_count; ++i) {
        REQUIRE(nce_program_prepare(&nce_programs[i], &active) == 0);
        NceProgram *p = &nce_programs[i];
        uintptr_t stub = p->base + p->span, end = stub + p->stub_size;
        REQUIRE(nce_program_index(p->base) == i && nce_program_index(end) == i + 1);
        REQUIRE(!nce_program_reserved(p->base, p->span));
        REQUIRE(nce_program_reserved(stub - 1, 2) && nce_program_reserved(end - 1, 1));
        REQUIRE(!nce_program_reserved(end, 1) && !nce_program_reserved(stub, 0));
    }
    REQUIRE(nce_program_reserved(ULONG_MAX, 2));
    printf("{\"test\":\"native_program_range_index\",\"passed\":true,\"programs\":%zu}\n", nce_program_count);
    REQUIRE(protect_none((void *)user_base, NCE_USER_SIZE + NCE_RAM_SIZE + NCE_VMALLOC_SIZE) == 0);
    nce_lkl_host.shmem_init = shmem_init;
    nce_lkl_host.shmem_reclaim = shmem_reclaim;
    nce_lkl_host.shmem_mmap = shmem_map;
    nce_lkl_host.munmap = protect_none;
    nce_lkl_host.user_clone = user_clone;
    nce_lkl_host.user_free = user_free;
    nce_lkl_host.user_switch = user_switch;
    nce_lkl_host.user_run = user_run;
    nce_lkl_host.user_exec = user_exec;
    nce_lkl_host.user_elf = nce_program_layout;
    nce_lkl_host.user_elf_offset = nce_program_source_offset;
    nce_lkl_host.user_reserved = nce_program_reserved;
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGSEGV, &action, &old_segv) == 0);
    REQUIRE(sigaction(SIGBUS, &action, &old_bus) == 0);
    REQUIRE(sigaction(SIGILL, &action, &old_ill) == 0);
    REQUIRE(sigaction(SIGTRAP, &action, &old_trap) == 0);
    REQUIRE(sigaction(SIGUSR2, &action, &old_preempt) == 0);
    event_resume = nce_memory_take(PAGE);
    REQUIRE(pthread_create(&timer_thread, NULL, preempt_timer, NULL) == 0);
    return user_base;
}
static void put_file(long (*call)(long, long *), const char *path, const void *data, size_t size)
{
    char parent[512];
    REQUIRE(strlen(path) < sizeof(parent));
    strcpy(parent, path);
    for (char *slash = parent + 1; *slash; ++slash) {
        if (*slash != '/')
            continue;
        *slash = 0;
        long mkdir_args[6] = {-100, (long)parent, 0755};
        long result = call(34, mkdir_args);
        REQUIRE(result == 0 || result == -17);
        *slash = '/';
    }
    long args[6] = {-100, (long)path, 0x242, 0700};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    args[0] = fd;
    args[1] = (long)data;
    args[2] = size;
    long written = call(64, args);
    if (written != (long)size)
        fprintf(stderr, "write guest file %s: wrote=%ld expected=%zu\n", path, written, size);
    REQUIRE(written == (long)size);
    REQUIRE(call(57, args) == 0);
}

// Files under /data must be installed after mounting the session data filesystem.
static void install_program_files(long (*call)(long, long *), bool data_files)
{
    for (size_t i = 0; i < nce_program_count; ++i) {
        NceProgram *program = &nce_programs[i];
        if (program->system_image || (!strncmp(program->path, "/data/", 6)) != data_files)
            continue;
        put_file(call, program->path, program->file, program->size);
        // Non-root exec also opens the common signal-return ELF during setup.
        long permissions[6] = {-100, (long)program->path, 0755};
        REQUIRE(call(53, permissions) == 0);
    }
}

static long get_file(long (*call)(long, long *), const char *path, char *buffer, size_t size)
{
    long args[6] = {-100, (long)path, 0};
    long fd = call(56, args);
    if (fd < 0)
        return fd;
    args[0] = fd;
    args[1] = (long)buffer;
    args[2] = size - 1;
    long count = call(63, args);
    REQUIRE(call(57, args) == 0);
    if (count >= 0)
        buffer[count] = 0;
    return count;
}

static void export_guest_file(long (*call)(long, long *), const char *guest, const char *host)
{
    long args[6] = {-100, (long)guest, 0};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    FILE *output = fopen(host, "wb");
    REQUIRE(output);
    char buffer[65536];
    args[0] = fd;
    args[1] = (long)buffer;
    args[2] = sizeof(buffer);
    long count;
    size_t total = 0;
    while ((count = call(63, args)) > 0) {
        REQUIRE(fwrite(buffer, 1, count, output) == (size_t)count);
        total += count;
    }
    REQUIRE(count == 0 && total > 0 && fclose(output) == 0);
    REQUIRE(call(57, args) == 0);
}

static void create_block_device(long (*call)(long, long *), const char *sysfs, const char *path)
{
    char device[64];
    unsigned major, minor;
    REQUIRE(get_file(call, sysfs, device, sizeof(device)) > 0);
    REQUIRE(sscanf(device, "%u:%u", &major, &minor) == 2 && major < 4096 && minor < 256);
    long args[6] = {-100, (long)path, 0060400, (major << 8) | minor};
    REQUIRE(call(33, args) == 0);
}

static void mount_system(long (*call)(long, long *))
{
    create_block_device(call, "/sys/block/ncedisk1/dev", "/dev/ncesystem");
    long args[6] = {0};
    const char *directories[] = {"/system-image", "/apex", "/system_ext", "/product", "/vendor"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i) {
        args[0] = -100;
        args[1] = (long)directories[i];
        args[2] = 0755;
        REQUIRE(call(34, args) == 0);
    }
    args[0] = (long)"/dev/ncesystem";
    args[1] = (long)"/system-image";
    args[2] = (long)"ext4";
    args[3] = 1;
    args[4] = 0;
    REQUIRE(call(40, args) == 0);
    const char *sources[] = {"/system-image/system", "/system-image/apex", "/system-image/system_ext", "/system-image/product", "/system-image/vendor"};
    const char *targets[] = {"/system", "/apex", "/system_ext", "/product", "/vendor"};
    for (unsigned i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
        args[0] = (long)sources[i];
        args[1] = (long)targets[i];
        args[2] = 0;
        args[3] = 4096;
        REQUIRE(call(40, args) == 0);
    }
    args[0] = -100;
    args[1] = (long)"/system/write-test";
    args[2] = 0x241;
    args[3] = 0600;
    REQUIRE(call(56, args) == -30);
    puts("{\"test\":\"android_system_image_readonly\",\"passed\":true}");
}

static void memory_report(long (*call)(long, long *), const char *phase)
{
    char info[4096];
    REQUIRE(get_file(call, "/proc/meminfo", info, sizeof(info)) > 0);
    unsigned long available = 0, shmem = 0;
    char *line = strstr(info, "MemAvailable:");
    REQUIRE(line && sscanf(line, "MemAvailable: %lu", &available) == 1);
    line = strstr(info, "Shmem:");
    REQUIRE(line && sscanf(line, "Shmem: %lu", &shmem) == 1);
    struct rusage usage;
    REQUIRE(getrusage(RUSAGE_SELF, &usage) == 0);
    struct task_vm_info vm;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    REQUIRE(task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) == KERN_SUCCESS);
    printf(
        "{\"memory\":\"%s\",\"linux_available_kib\":%lu,\"linux_shmem_kib\":%lu,"
        "\"host_peak_rss_bytes\":%ld,\"host_footprint_bytes\":%llu,\"reclaimed_bytes\":%lu,\"reclaimed_ranges\":%lu}\n",
        phase, available, shmem, usage.ru_maxrss, vm.phys_footprint, reclaimed_bytes, reclaimed_ranges);
}

static void mount_android_data(long (*call)(long, long *))
{
    long args[6] = {-100, (long)"/data", 0771};
    long result = call(34, args);
    REQUIRE(result == 0 || result == -17);
    args[0] = (long)"/dev/ncedisk0";
    args[1] = (long)"/data";
    args[2] = (long)"ext4";
    args[3] = 1024;
    args[4] = 0;
    REQUIRE(call(40, args) == 0);
    args[0] = -100;
    args[1] = (long)"/data";
    args[2] = 1000;
    args[3] = 1000;
    REQUIRE(call(54, args) == 0);
    args[2] = 0771;
    REQUIRE(call(53, args) == 0);
    args[1] = (long)"/data/nce";
    args[2] = 0755;
    result = call(34, args);
    REQUIRE(result == 0 || result == -17);
    // Probe flags and generated fixtures start fresh; Android app data persists.
    args[0] = (long)"tmpfs";
    args[1] = (long)"/data/nce";
    args[2] = (long)"tmpfs";
    args[3] = 0;
    args[4] = (long)"mode=0755";
    REQUIRE(call(40, args) == 0);
    puts("{\"test\":\"android_data_ext4_mounted\",\"passed\":true}");
}

static void dump_file(long (*call)(long, long *), const char *path)
{
    long args[6] = {-100, (long)path, 0};
    long fd = call(56, args);
    if (fd < 0)
        return;
    char buffer[4096];
    long count;
    args[0] = fd;
    args[1] = (long)buffer;
    args[2] = sizeof(buffer);
    while ((count = call(63, args)) > 0)
        fwrite(buffer, 1, count, stderr);
    REQUIRE(call(57, args) == 0);
}

struct LogPump {
    long (*call)(long, long *);
    atomic_int stopping;
};

static void report_mmu_profile(void)
{
    if (!map_profile)
        return;
    fprintf(stderr, "{\"profile\":\"linux_mmu\",\"switches\":%lu,\"scanned_pages\":%lu,\"present_faults\":%lu,\"linux_faults\":%lu,\"fast_faults\":%lu,\"fast_restored_pages\":%lu,\"restored_pages\":%lu}\n",
            __atomic_load_n(&mmu_profile.switches, __ATOMIC_RELAXED),
            __atomic_load_n(&mmu_profile.scanned_pages, __ATOMIC_RELAXED),
            __atomic_load_n(&mmu_profile.present_faults, __ATOMIC_RELAXED),
            __atomic_load_n(&mmu_profile.linux_faults, __ATOMIC_RELAXED),
            __atomic_load_n(&mmu_profile.fast_faults, __ATOMIC_RELAXED),
            __atomic_load_n(&mmu_profile.fast_restored_pages, __ATOMIC_RELAXED),
            __atomic_load_n(&mmu_profile.restored_pages, __ATOMIC_RELAXED));
}

static void *stream_guest_log(void *opaque)
{
    struct LogPump *pump = opaque;
    char buffer[65536];
    long offsets[2] = {0, 0};
    uint64_t begin = profile_clock(), report = begin;
    unsigned long initial_calls[MAP_OPERATIONS], initial_time[MAP_OPERATIONS];
    for (unsigned i = 0; i < MAP_OPERATIONS; ++i) {
        initial_calls[i] = atomic_load_explicit(&map_calls[i], memory_order_relaxed);
        initial_time[i] = atomic_load_explicit(&map_time_ns[i], memory_order_relaxed);
    }
    for (;;) {
        uint64_t now = profile_clock();
        if (map_profile && now - report >= 10000000000ULL) {
            fprintf(stderr, "{\"profile\":\"host_mapping\",\"elapsed_ms\":%.3f,\"data_calls\":%lu,\"data_ms\":%.3f,\"code_calls\":%lu,\"code_ms\":%.3f,\"revoke_calls\":%lu,\"revoke_ms\":%.3f}\n",
                    (now - begin) / 1e6,
                    atomic_load_explicit(&map_calls[MAP_DATA], memory_order_relaxed) - initial_calls[MAP_DATA],
                    (atomic_load_explicit(&map_time_ns[MAP_DATA], memory_order_relaxed) - initial_time[MAP_DATA]) / 1e6,
                    atomic_load_explicit(&map_calls[MAP_CODE], memory_order_relaxed) - initial_calls[MAP_CODE],
                    (atomic_load_explicit(&map_time_ns[MAP_CODE], memory_order_relaxed) - initial_time[MAP_CODE]) / 1e6,
                    atomic_load_explicit(&map_calls[MAP_REVOKE], memory_order_relaxed) - initial_calls[MAP_REVOKE],
                    (atomic_load_explicit(&map_time_ns[MAP_REVOKE], memory_order_relaxed) - initial_time[MAP_REVOKE]) / 1e6);
            report = now;
            fprintf(stderr, "{\"profile\":\"resume_entries\",\"hits\":%lu,\"misses\":%lu}\n",
                    atomic_load_explicit(&resume_hits, memory_order_relaxed),
                    atomic_load_explicit(&resume_misses, memory_order_relaxed));
            report_mmu_profile();
        }
        bool received = false;
        for (unsigned stream = 0; stream < 2; ++stream) {
            long args[6] = {stream + 1, (long)buffer, sizeof(buffer), offsets[stream]};
            long count = pump->call(67, args);
            REQUIRE(count >= 0);
            if (!count)
                continue;
            FILE *output = stream ? stderr : stdout;
            REQUIRE(fwrite(buffer, 1, count, output) == (size_t)count);
            REQUIRE(fflush(output) == 0);
            offsets[stream] += count;
            received = true;
        }
        if (received)
            continue;
        if (atomic_load(&pump->stopping))
            return NULL;
        struct timespec delay = {.tv_sec = 1};
        nanosleep(&delay, NULL);
    }
}

static void run_program(long (*call)(long, long *), const char *path, const char *input)
{
    if (input)
        put_file(call, "/stdin", input, strlen(input));
    const char *streams[] = {"/stdin", "/stdout", "/stderr"};
    for (long fd = 0; fd < 3; ++fd) {
        long args[6] = {-100, (long)streams[fd], fd ? 0x242 : 0x40, 0600};
        long opened = call(56, args);
        REQUIRE(opened >= 0);
        if (opened != fd) {
            args[0] = opened;
            args[1] = fd;
            args[2] = 0;
            REQUIRE(call(24, args) == fd);
            REQUIRE(call(57, args) == 0);
        }
    }
    Process *process = calloc(1, sizeof(*process));
    REQUIRE(process);
    ++allocated;
    process->state.syscall = syscall_from_guest;
    process->exec_path = path;
    struct LogPump pump = {.call = call, .stopping = 0};
    bool live_log = !strcmp(path, "/system/bin/android-services-test") || !strcmp(path, "/system/bin/art-compiler-test");
    pthread_t log_thread;
    if (live_log)
        REQUIRE(pthread_create(&log_thread, NULL, stream_guest_log, &pump) == 0);
    if (live_log && getenv("NCE_DESKTOP"))
        nce_session_set_syscall(call);
    long args[6] = {(long)process, 1};
    long pid = call(246, args);
    REQUIRE(pid > 0);
    int status = -1;
    args[0] = pid;
    args[1] = (long)&status;
    args[2] = 0;
    long waited = call(260, args);
    if (live_log) {
        nce_session_set_syscall(NULL);
        atomic_store(&pump.stopping, 1);
        REQUIRE(pthread_join(log_thread, NULL) == 0);
    } else
        dump_file(call, "/stderr");
    if (status || waited != pid) {
        dump_file(call, "/stdout");
        if (live_log) {
            dump_file(call, "/data/nce/window-state.txt");
            dump_file(call, "/data/nce/activity-state.txt");
        }
        dump_file(call, "/proc/meminfo");
        fprintf(stderr, "userspace pid=%ld waited=%ld status=%d\n", pid, waited, status);
        char messages[16384];
        long logs[6] = {3, (long)messages, sizeof(messages)};
        long bytes = call(116, logs);
        if (bytes > 0)
            fwrite(messages, 1, bytes, stderr);
    }
    REQUIRE(waited == pid && status == 0);
    for (long fd = 0; fd < 3; ++fd) {
        args[0] = fd;
        REQUIRE(call(57, args) == 0);
    }
}

static void check_program_files(long (*call)(long, long *), bool data_files)
{
    for (size_t i = 1; i < nce_program_count; ++i) {
        NceProgram *p = &nce_programs[i];
        if ((!strncmp(p->path, "/data/", 6)) != data_files)
            continue;
        unsigned char *bytes = malloc(p->size + 1);
        REQUIRE(bytes);
        if (p->source_offset) {
            long args[6] = {-100, (long)p->container, 0};
            long fd = call(56, args);
            REQUIRE(fd >= 0);
            args[0] = fd;
            args[1] = (long)bytes;
            args[2] = p->size;
            args[3] = p->source_offset;
            REQUIRE(call(67, args) == (long)p->size && call(57, args) == 0);
        } else {
            REQUIRE(get_file(call, p->container, (void *)bytes, p->size + 1) == (long)p->size);
        }
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        REQUIRE(p->size <= UINT32_MAX);
        CC_SHA256(bytes, (CC_LONG)p->size, digest);
        REQUIRE(!memcmp(digest, p->sha256, sizeof(digest)));
        free(bytes);
    }
}

static void reclaim_file_cache(long (*call)(long, long *))
{
    char info[4096];
    unsigned long before, after;
    REQUIRE(get_file(call, "/proc/meminfo", info, sizeof(info)) > 0);
    char *line = strstr(info, "Cached:");
    REQUIRE(line && sscanf(line, "Cached: %lu", &before) == 1);
    long args[6] = {0};
    REQUIRE(call(81, args) == 0);
    args[0] = -100;
    args[1] = (long)"/proc/sys/vm/drop_caches";
    args[2] = 1;
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    args[0] = fd;
    args[1] = (long)"3\n";
    args[2] = 2;
    REQUIRE(call(64, args) == 2 && call(57, args) == 0);
    REQUIRE(get_file(call, "/proc/meminfo", info, sizeof(info)) > 0);
    line = strstr(info, "Cached:");
    REQUIRE(line && sscanf(line, "Cached: %lu", &after) == 1);
    REQUIRE(before > after + 1024);
    check_program_files(call, false);
    printf("{\"test\":\"android_system_cache_reclaim\",\"before_kib\":%lu,\"after_kib\":%lu,\"passed\":true}\n", before, after);
}

static void userspace_tests(long (*call)(long, long *))
{
    struct timespec begin, end;
    unsigned long previous_stackless = stackless_exits;
    unsigned long previous_preemptions = preemptions;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &begin) == 0);
    put_file(call, "/unregistered-elf", nce_programs[2].file, nce_programs[2].size);
    const char *script =
        "/worker check\n/worker check\n/worker check\n/worker check\n"
        "/worker check\n/worker check\n/worker check\n/worker check\n";
    put_file(call, "/boot.script", script, strlen(script));
    puts("{\"phase\":\"linux_userspace_tests\"}");
    run_program(call, "/init-user", NULL);
    char buffer[4096];
    REQUIRE(get_file(call, "/userspace.result", buffer, sizeof(buffer)) > 0);
    REQUIRE(strstr(buffer, "init pid=1 commands=8 libc=musl passed"));
    fprintf(stderr, "%s", buffer);
    check_program_files(call, false);
    puts("{\"test\":\"linux_musl_static_pie_processes\",\"passed\":true,\"commands\":8,\"cow_bytes_per_command\":2097152}");
    puts("{\"test\":\"linux_compiled_elf_files_unchanged\",\"passed\":true}");
    REQUIRE(preemptions > previous_preemptions && deferred_faults > 0 && stackless_exits - previous_stackless == 8);
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &end) == 0);
    double ms = (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1e6;
    printf("{\"test\":\"linux_musl_threads_preemption\",\"passed\":true,\"threads\":56,\"stackless_exits\":%lu,\"elapsed_ms\":%.3f}\n", stackless_exits - previous_stackless, ms);
    task_vm_info_data_t vm;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) == KERN_SUCCESS && count >= TASK_VM_INFO_REV1_COUNT) {
        printf("{\"memory\":\"after_userspace\",\"phys_footprint\":%llu,\"resident_peak\":%llu}\n", vm.phys_footprint, vm.resident_size_peak);
    }
}

static void *send_input_from_host_thread(void *unused)
{
    (void)unused;
    REQUIRE(nce_host_input_send(NCE_INPUT_DOWN, 359, 639) == 0);
    REQUIRE(nce_host_input_send(NCE_INPUT_KEY, 158, 0) == 0);
    REQUIRE(nce_host_input_send(NCE_INPUT_MOVE, 360, 0) == -22);
    REQUIRE(nce_host_input_send(NCE_INPUT_KEY, 999, 0) == -22);
    return NULL;
}

static void host_input_test(long (*call)(long, long *))
{
    REQUIRE(nce_host_input_send(NCE_INPUT_DOWN, 0, 0) == -19);
    long args[6] = {1, 2 | 0x80000};
    long fd = call(198, args);
    REQUIRE(fd >= 0);
    struct {
        uint16_t family;
        char path[108];
    } address = {1, "/dev/nce-input"};
    args[0] = fd;
    args[1] = (long)&address;
    args[2] = sizeof(address);
    REQUIRE(call(200, args) == 0);
    nce_host_input_start(call);
    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, send_input_from_host_thread, NULL) == 0);
    REQUIRE(pthread_join(thread, NULL) == 0);
    const NceInputCommand expected[] = {{NCE_INPUT_DOWN, 359, 639}, {NCE_INPUT_KEY, 158, 0}};
    NceInputCommand command;
    args[0] = fd;
    args[1] = (long)&command;
    args[2] = sizeof(command);
    args[3] = 0x40;
    for (unsigned i = 0; i < 2; ++i) {
        REQUIRE(call(207, args) == sizeof(command));
        REQUIRE(memcmp(&command, &expected[i], sizeof(command)) == 0);
    }
    REQUIRE(call(207, args) == -11);
    nce_host_input_stop();
    REQUIRE(nce_host_input_send(NCE_INPUT_UP, 359, 639) == -19);
    args[0] = fd;
    REQUIRE(call(57, args) == 0);
    args[0] = -100;
    args[1] = (long)address.path;
    args[2] = 0;
    REQUIRE(call(35, args) == 0);
    puts("{\"test\":\"linux_host_input_thread_protocol\",\"passed\":true}");
}

struct HostPoll {
    long (*call)(long, long *);
    long fd;
    double elapsed_ms;
};

static void *poll_busy_guest(void *opaque)
{
    struct HostPoll *probe = opaque;
    uint32_t state[4] = {0};
    long args[6] = {0};
    long fd = probe->fd;
    struct timespec start, end;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &start) == 0);
    for (uint32_t round = 0; round < 32; ++round) {
        do {
            args[0] = fd;
            args[1] = (long)state;
            args[2] = sizeof(state);
            args[3] = 0;
            REQUIRE(probe->call(67, args) == sizeof(state));
            REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &end) == 0);
            probe->elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
            REQUIRE(probe->elapsed_ms < 2000);
            if (!state[0] || state[2] != round) {
                struct timespec delay = {.tv_nsec = 1000000};
                nanosleep(&delay, NULL);
            }
        } while (!state[0] || state[2] != round);
        uint32_t request = round + 1;
        args[0] = fd;
        args[1] = (long)&request;
        args[2] = sizeof(request);
        args[3] = 4;
        REQUIRE(probe->call(68, args) == sizeof(request));
    }
    args[0] = fd;
    REQUIRE(probe->call(57, args) == 0);
    return NULL;
}

static void process_regressions(long (*host_syscall)(long, long *))
{
    code = nce_memory_take(65536);
    puts("{\"phase\":\"linux_process_tests\"}");
    const unsigned char *exec_elf = nce_programs[0].file;
    size_t exec_size = nce_programs[0].size;
    long file_args[6] = {0}, fd;
    struct timespec begin, end;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &begin) == 0);
    long args[6] = {0};
    const unsigned rounds = 32;
    for (unsigned round = 0; round < rounds; ++round) {
        NceState *state = calloc(1, sizeof(Process));
        REQUIRE(state);
        ++allocated;
        state->syscall = syscall_from_guest;
        state->regs.x[21] = USER_BASE + NCE_MMAP_OFFSET;
        REQUIRE(nce_prepare_shared(code, state, &active, guest_image, sizeof(guest_image), GUEST_LINUX_PROCESS) == 0);
        args[0] = (long)state;
        args[1] = 0;
        long pid = host_syscall(246, args);
        REQUIRE(pid > 0);
        int status = -1;
        args[0] = pid;
        args[1] = (long)&status;
        args[2] = 0;
        args[3] = 0;
        long waited = host_syscall(260, args);
        if (status)
            fprintf(stderr, "round=%u pid=%ld waited=%ld status=%d\n", round, pid, waited, status);
        REQUIRE(waited == pid && status == 0);
    }
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &end) == 0);
    double elapsed_ms = (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1e6;
    REQUIRE(allocated == rounds * 3);
    printf("{\"test\":\"linux_nce_fork_cow_shared_futex_pipe_exec_wait\",\"passed\":true,\"rounds\":%u,\"faults\":%lu,\"elapsed_ms\":%.3f}\n", rounds, faults, elapsed_ms);
    REQUIRE(faults >= rounds);
    puts("{\"test\":\"linux_exec_then_fork_cow_wait\",\"passed\":true}");
    file_args[0] = -100;
    file_args[1] = (long)"/nce-exec";
    file_args[2] = 0;
    fd = host_syscall(56, file_args);
    REQUIRE(fd >= 0);
    unsigned char *original = malloc(exec_size);
    REQUIRE(original);
    file_args[0] = fd;
    file_args[1] = (long)original;
    file_args[2] = exec_size;
    REQUIRE(host_syscall(63, file_args) == (long)exec_size);
    REQUIRE(memcmp(original, exec_elf, exec_size) == 0);
    free(original);
    REQUIRE(host_syscall(57, file_args) == 0);
    puts("{\"test\":\"linux_exec_file_unchanged\",\"passed\":true}");
    REQUIRE(host_syscall(-1, args) == -38 && host_syscall(99999, args) == -38);
    puts("{\"test\":\"linux_syscall_bounds\",\"passed\":true}");
    unsigned stress_rounds = 16;
    const char *requested_rounds = getenv("NCE_STRESS_ROUNDS");
    if (requested_rounds) {
        char *end;
        unsigned long value = strtoul(requested_rounds, &end, 10);
        REQUIRE(*requested_rounds && !*end && value >= 1 && value <= 1024);
        stress_rounds = (unsigned)value;
    }
    for (unsigned iteration = 0; iteration < stress_rounds; ++iteration) {
        printf("{\"phase\":\"linux_userspace_round\",\"round\":%u}\n", iteration);
        userspace_tests(host_syscall);
    }
    printf("{\"test\":\"linux_userspace_stress\",\"passed\":true,\"rounds\":%u,\"commands\":%u,\"threads\":%u}\n",
           stress_rounds, stress_rounds * 8, stress_rounds * 56);
    puts("{\"phase\":\"linux_mm_benchmark\"}");
    unsigned long maps_before = data_maps, copied_before = code_copy_bytes, revoked_before = revocations;
    run_program(host_syscall, "/mm-bench", NULL);
    char benchmark[16384];
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_mm_exchange_integrity"));
    fputs(benchmark, stdout);
    printf("{\"test\":\"linux_mm_host_operations\",\"data_maps\":%lu,\"code_copy_bytes\":%lu,\"revocations\":%lu}\n",
           data_maps - maps_before, code_copy_bytes - copied_before, revocations - revoked_before);
    puts("{\"phase\":\"linux_dash_shell\"}");
    run_program(host_syscall, "/bin/sh", shell_script);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_dash_shell_suite"));
    fputs(benchmark, stdout);
    puts("{\"phase\":\"linux_dynamic_program\"}");
    run_program(host_syscall, "/dynamic", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_dynamic_fork_signal"));
    fputs(benchmark, stdout);
    puts("{\"phase\":\"linux_storage_program\"}");
    run_program(host_syscall, "/storage-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_ext4_persistence"));
    fputs(benchmark, stdout);
}

static void android_abi_regressions(long (*host_syscall)(long, long *))
{
    char benchmark[16384];
    REQUIRE(nce_session_request_stop() == -19);
    nce_session_set_syscall(host_syscall);
    REQUIRE(nce_session_request_stop() == 0 && nce_session_request_stop() == 0);
    nce_session_set_syscall(NULL);
    REQUIRE(nce_session_request_stop() == -19);
    REQUIRE(get_file(host_syscall, "/data/nce/session-stop", benchmark, sizeof(benchmark)) == 0);
    long stop_args[6] = {-100, (long)"/data/nce/session-stop", 0};
    REQUIRE(host_syscall(35, stop_args) == 0);
    puts("{\"test\":\"linux_session_stop_lifecycle\",\"passed\":true}");
    uint32_t host_poll_state[4] = {0};
    put_file(host_syscall, "/data/nce/host-scheduling", host_poll_state, sizeof(host_poll_state));
    struct HostPoll host_poll = {.call = host_syscall};
    long poll_args[6] = {-100, (long)"/data/nce/host-scheduling", 2};
    long poll_file = host_syscall(56, poll_args);
    REQUIRE(poll_file >= 0);
    poll_args[0] = poll_file;
    poll_args[1] = 1030;
    poll_args[2] = 16;
    host_poll.fd = host_syscall(25, poll_args);
    REQUIRE(host_poll.fd >= 16 && host_syscall(57, poll_args) == 0);
    pthread_t host_poll_thread;
    REQUIRE(pthread_create(&host_poll_thread, NULL, poll_busy_guest, &host_poll) == 0);
    run_program(host_syscall, "/host-poll-test", NULL);
    REQUIRE(pthread_join(host_poll_thread, NULL) == 0);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_busy_guest_host_requests"));
    fputs(benchmark, stdout);
    printf("{\"benchmark\":\"linux_busy_guest_host_requests\",\"elapsed_ms\":%.3f}\n", host_poll.elapsed_ms);
    puts("{\"phase\":\"linux_binder_program\"}");
    run_program(host_syscall, "/binder-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_binder_invalid_handle_cleanup"));
    fputs(benchmark, stdout);
    puts("{\"phase\":\"android_bionic_dynamic\"}");
    run_program(host_syscall, "/bionic-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "android_bionic_threads_allocator_tls"));
    fputs(benchmark, stdout);
}

#ifdef NCE_GRAPHICS
static void graphics_regressions(long (*host_syscall)(long, long *))
{
    char benchmark[16384];
    run_program(host_syscall, "/system/bin/gpu-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_native_gles_metal"));
    REQUIRE(strstr(benchmark, "linux_native_gles_shader_frames"));
    fputs(benchmark, stdout);
    run_program(host_syscall, "/system/bin/egl-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "android_sdk_egl_driver_pixels"));
    fputs(benchmark, stdout);
    const char *gpu_output = getenv("NCE_GPU_OUTPUT");
    if (gpu_output) {
        size_t capacity = NCE_GPU_WIDTH * NCE_GPU_HEIGHT * 4 + 55;
        void *pixels = malloc(capacity);
        REQUIRE(pixels);
        long bytes = get_file(host_syscall, "/data/nce/gpu.bmp", pixels, capacity);
        REQUIRE(bytes == (long)capacity - 1 && !memcmp(pixels, "BM", 2));
        FILE *file = fopen(gpu_output, "wb");
        REQUIRE(file && fwrite(pixels, 1, bytes, file) == (size_t)bytes && fclose(file) == 0);
        free(pixels);
    }
}
#endif

static void art_regressions(long (*host_syscall)(long, long *))
{
    char benchmark[16384];
    run_program(host_syscall, "/system/bin/cfi-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "android_cfi_library_reload"));
    fputs(benchmark, stdout);
    puts("{\"phase\":\"android_art_reference_heap\"}");
    run_program(host_syscall, "/art-heap-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "art_high_reference_heap"));
    fputs(benchmark, stdout);
    memory_report(host_syscall, "before_art");
    puts("{\"phase\":\"android_art_load\"}");
    run_program(host_syscall, "/system/bin/art-test", "");
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "android_art_library_loaded"));
    REQUIRE(strstr(benchmark, "android_art_java_workload"));
    REQUIRE(strstr(benchmark, "android_art_jit_arithmetic"));
    REQUIRE(strstr(benchmark, "android_art_jit_objects"));
    REQUIRE(strstr(benchmark, "android_art_jit_calls"));
    REQUIRE(strstr(benchmark, "android_art_jit_allocations"));
    REQUIRE(strstr(benchmark, "android_art_jit_automatic"));
    REQUIRE(strstr(benchmark, "android_art_jit_runtime_features"));
    REQUIRE(strstr(benchmark, "android_art_jit_patterns"));
    REQUIRE(strstr(benchmark, "android_art_jit_strings_classes"));
    REQUIRE(strstr(benchmark, "android_art_jit_types"));
    REQUIRE(strstr(benchmark, "android_art_jit_intrinsics"));
    REQUIRE(strstr(benchmark, "android_art_intrinsics_benchmark"));
    REQUIRE(strstr(benchmark, "android_art_proxy_references"));
    REQUIRE(strstr(benchmark, "android_art_vm_destroyed"));
    fputs(benchmark, stdout);
    puts("{\"phase\":\"android_art_inlining\"}");
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        run_program(host_syscall, "/system/bin/art-test", enabled ? "inline:32\n" : "inline:0\n");
        REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
        REQUIRE(strstr(benchmark, "android_art_jit_inlining"));
        REQUIRE(strstr(benchmark, "android_art_vm_destroyed"));
        fputs(benchmark, stdout);
    }
    memory_report(host_syscall, "after_art");
}

static void reclaim_regressions(long (*host_syscall)(long, long *))
{
    char benchmark[16384];
    check_program_files(host_syscall, false);
    reclaim_file_cache(host_syscall);
    long reclaim_args[6] = {-100, (long)"/proc/sys/vm/drop_caches", 1};
    long reclaim_fd = host_syscall(56, reclaim_args);
    REQUIRE(reclaim_fd >= 0);
    reclaim_args[0] = reclaim_fd;
    reclaim_args[1] = (long)"3\n";
    reclaim_args[2] = 2;
    REQUIRE(host_syscall(64, reclaim_args) == 2 && host_syscall(57, reclaim_args) == 0);
    struct timespec reclaim_delay = {3, 0};
    reclaim_args[0] = (long)&reclaim_delay;
    reclaim_args[1] = 0;
    REQUIRE(host_syscall(101, reclaim_args) == 0 && reclaimed_ranges > 0);
    memory_report(host_syscall, "after_host_reclaim");
    run_program(host_syscall, "/reclaim-test", NULL);
    REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
    REQUIRE(strstr(benchmark, "linux_reclaimed_ram_reuse"));
    fputs(benchmark, stdout);
    puts("{\"test\":\"linux_dynamic_elf_files_unchanged\",\"passed\":true}");
}

void nce_process_run(long (*host_syscall)(long, long *), long (*guest_syscall)(long, long *),
                     int (*handle_fault)(unsigned long, int),
                     void (*return_to_user)(long, long, unsigned long), void (*signal_fault)(unsigned long, int),
                     void (*preempt_user)(void), void (*prepare_user_memory)(void))
{
    const bool interactive = getenv("NCE_DESKTOP") != NULL;
    char benchmark[16384];
    run_syscall = guest_syscall;
    page_fault = handle_fault;
    user_return = return_to_user;
    user_fault = signal_fault;
    user_preempt = preempt_user;
    prepare_user = prepare_user_memory;
    long mkdir_args[6] = {-100, (long)"/bin", 0755};
    REQUIRE(host_syscall(34, mkdir_args) == 0);
    mkdir_args[1] = (long)"/lib";
    REQUIRE(host_syscall(34, mkdir_args) == 0);
    const char *directories[] = {"/system", "/system/bin", "/system/lib64", "/system/bin/bootstrap", "/proc", "/sys"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i) {
        mkdir_args[1] = (long)directories[i];
        REQUIRE(host_syscall(34, mkdir_args) == 0);
    }
    long mount_args[6] = {(long)"proc", (long)"/proc", (long)"proc", 0, 0};
    REQUIRE(host_syscall(40, mount_args) == 0);
    mount_args[0] = (long)"sysfs";
    mount_args[1] = (long)"/sys";
    mount_args[2] = (long)"sysfs";
    REQUIRE(host_syscall(40, mount_args) == 0);
    mount_args[0] = (long)"selinuxfs";
    mount_args[1] = (long)"/sys/fs/selinux";
    mount_args[2] = (long)"selinuxfs";
    REQUIRE(host_syscall(40, mount_args) == 0);
    mount_args[0] = (long)"fusectl";
    mount_args[1] = (long)"/sys/fs/fuse/connections";
    mount_args[2] = (long)"fusectl";
    REQUIRE(host_syscall(40, mount_args) == 0);
    long fuse_args[6] = {-100, (long)"/dev/fuse", 0020600, (10 << 8) | 229};
    REQUIRE(host_syscall(33, fuse_args) == 0);
    long link_args[6] = {(long)"/system/bin/linker64", -100, (long)"/system/bin/bootstrap/linker64"};
    REQUIRE(host_syscall(36, link_args) == 0);
    long null_args[6] = {-100, (long)"/dev/null", 0020666, 259};
    REQUIRE(host_syscall(33, null_args) == 0);
    null_args[1] = (long)"/dev/kmsg";
    null_args[3] = 267;
    REQUIRE(host_syscall(33, null_args) == 0);
    null_args[1] = (long)"/dev/random";
    null_args[3] = 264;
    REQUIRE(host_syscall(33, null_args) == 0);
    null_args[1] = (long)"/dev/urandom";
    null_args[3] = 265;
    REQUIRE(host_syscall(33, null_args) == 0);
    for (const char **path = (const char *[]){"/dev/null", "/dev/random", "/dev/urandom", NULL}; *path; ++path) {
        long mode_args[6] = {-100, (long)*path, 0666};
        REQUIRE(host_syscall(53, mode_args) == 0);
    }
    struct {
        int entropy_bits, bytes;
        unsigned char data[32];
    } entropy = {256, 32, {0}};
    arc4random_buf(entropy.data, sizeof(entropy.data));
    long random_args[6] = {-100, (long)"/dev/random", 2};
    long random_fd = host_syscall(56, random_args);
    REQUIRE(random_fd >= 0);
    random_args[0] = random_fd;
    random_args[1] = 0x40085203;
    random_args[2] = (long)&entropy;
    REQUIRE(host_syscall(29, random_args) == 0);
    REQUIRE(host_syscall(57, random_args) == 0);
    memset(entropy.data, 0, sizeof(entropy.data));
    random_args[0] = (long)entropy.data;
    random_args[1] = sizeof(entropy.data);
    random_args[2] = 1;
    REQUIRE(host_syscall(278, random_args) == sizeof(entropy.data));
    puts("{\"test\":\"linux_random_initialized\",\"passed\":true}");
    mount_system(host_syscall);
    create_block_device(host_syscall, "/sys/block/ncedisk0/dev", "/dev/ncedisk0");
    install_program_files(host_syscall, false);
    if (!interactive)
        host_input_test(host_syscall);
    memory_report(host_syscall, "system_mounted");
    if (!interactive)
        process_regressions(host_syscall);
    mount_android_data(host_syscall);
    install_program_files(host_syscall, true);
    check_program_files(host_syscall, true);
    const char *nterp = getenv("NCE_ART_NTERP");
    REQUIRE(!nterp || !strcmp(nterp, "0") || !strcmp(nterp, "1"));
    put_file(host_syscall, "/data/nce/art-nterp", nterp ? nterp : "0", 1);
    long nterp_permissions[6] = {-100, (long)"/data/nce/art-nterp", 0644};
    REQUIRE(host_syscall(53, nterp_permissions) == 0);
    printf("{\"config\":\"art_nterp\",\"requested\":%s}\n", nterp ? nterp : "0");
    const char *expanded_jit = getenv("NCE_ART_EXPANDED_JIT");
    REQUIRE(!expanded_jit || !strcmp(expanded_jit, "0") || !strcmp(expanded_jit, "1"));
    put_file(host_syscall, "/data/nce/art-expanded-jit", expanded_jit ? expanded_jit : "0", 1);
    long jit_permissions[6] = {-100, (long)"/data/nce/art-expanded-jit", 0644};
    REQUIRE(host_syscall(53, jit_permissions) == 0);
    printf("{\"config\":\"art_expanded_jit\",\"requested\":%s}\n", expanded_jit ? expanded_jit : "0");
    const char *fast_baseline = getenv("NCE_ART_FAST_BASELINE");
    REQUIRE(!fast_baseline || !strcmp(fast_baseline, "0") || !strcmp(fast_baseline, "1"));
    put_file(host_syscall, "/data/nce/art-fast-baseline", fast_baseline ? fast_baseline : "0", 1);
    long fast_permissions[6] = {-100, (long)"/data/nce/art-fast-baseline", 0644};
    REQUIRE(host_syscall(53, fast_permissions) == 0);
    printf("{\"config\":\"art_fast_baseline\",\"requested\":%s}\n", fast_baseline ? fast_baseline : "0");
    const char *aot = getenv("NCE_ART_AOT");
    REQUIRE(!aot || !strcmp(aot, "0") || !strcmp(aot, "1"));
    put_file(host_syscall, "/data/nce/art-aot", aot ? aot : "1", 1);
    long aot_permissions[6] = {-100, (long)"/data/nce/art-aot", 0644};
    REQUIRE(host_syscall(53, aot_permissions) == 0);
    printf("{\"config\":\"art_aot\",\"requested\":%s}\n", aot ? aot : "1");
    put_file(host_syscall, "/data/nce/art-compiler-profile", map_profile ? "1" : "0", 1);
    long compiler_profile_permissions[6] = {-100, (long)"/data/nce/art-compiler-profile", 0644};
    REQUIRE(host_syscall(53, compiler_profile_permissions) == 0);
    if (!interactive)
        android_abi_regressions(host_syscall);
#ifdef NCE_GRAPHICS
    puts("{\"phase\":\"native_graphics_start\"}");
    REQUIRE(nce_graphics_start(host_syscall) == 0);
    put_file(host_syscall, "/data/nce/graphics", "1", 1);
    if (!interactive)
        graphics_regressions(host_syscall);
#endif
    for (size_t i = 0; i < sizeof(art_assets) / sizeof(art_assets[0]); ++i) {
        put_file(host_syscall, art_assets[i].path, art_assets[i].data, art_assets[i].size);
        long permissions[6] = {-100, (long)art_assets[i].path, 0644};
        REQUIRE(host_syscall(53, permissions) == 0);
    }
    put_file(host_syscall, "/data/dalvik-cache/arm64/.keep", "", 0);
    if (getenv("NCE_ART_COMPILER_PROBE")) {
        puts("{\"phase\":\"android_art_compiler\"}");
        run_program(host_syscall, "/system/bin/art-compiler-test", "");
        REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
        REQUIRE(strstr(benchmark, "android_art_compiler_verify"));
        REQUIRE(strstr(benchmark, "android_art_compiler_invalid_input"));
        memory_report(host_syscall, "after_art_compiler");
        run_program(host_syscall, "/system/bin/art-test", "oat-verify\n");
        REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
        REQUIRE(strstr(benchmark, "android_art_compiler_runtime_load"));
        REQUIRE(strstr(benchmark, "android_art_java_workload"));
        REQUIRE(strstr(benchmark, "android_art_vm_destroyed"));
        fputs(benchmark, stdout);
        run_program(host_syscall, "/system/bin/art-compiler-test", "speed\n");
        REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
        REQUIRE(strstr(benchmark, "android_art_compiler_speed"));
        const char *output = getenv("NCE_ART_COMPILER_OUTPUT");
        REQUIRE(output);
        bool desktop = getenv("NCE_ART_COMPILER_DESKTOP") != NULL;
        if (getenv("NCE_ART_COMPILER_BOOT")) {
            run_program(host_syscall, "/system/bin/art-compiler-test", "boot\n");
            REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
            REQUIRE(strstr(benchmark, "android_art_boot_image_generated"));
            const char *extensions[] = {"art", "oat", "vdex"};
            for (size_t i = 0; i < 3; ++i) {
                char guest[512], host[4096];
                snprintf(guest, sizeof(guest), "/data/nce/boot/arm64/boot.%s", extensions[i]);
                int length = snprintf(host, sizeof(host), "%s/boot.%s", output, extensions[i]);
                REQUIRE(length > 0 && (size_t)length < sizeof(host));
                export_guest_file(host_syscall, guest, host);
            }
        }
        if (desktop) {
            run_program(host_syscall, "/system/bin/art-compiler-test", "desktop\n");
            REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
            REQUIRE(strstr(benchmark, "android_art_compiler_desktop"));
        }
        const char *extensions[] = {"odex", "vdex"};
        size_t targets = desktop ? sizeof(aot_desktop_targets) / sizeof(aot_desktop_targets[0]) : 1;
        for (size_t i = 0; i < targets; ++i) {
            const char *name = desktop ? aot_desktop_targets[i].name : "NceWorkload";
            for (size_t j = 0; j < 2; ++j) {
                char guest[512], host[4096];
                snprintf(guest, sizeof(guest), "/data/nce/oat/arm64/%s.%s", name, extensions[j]);
                int length = snprintf(host, sizeof(host), "%s/%s.%s", output, name, extensions[j]);
                REQUIRE(length > 0 && (size_t)length < sizeof(host));
                export_guest_file(host_syscall, guest, host);
            }
        }
    } else if (getenv("NCE_ART_BOOT_PROBE")) {
        const char *modes[] = {"boot-image\n", "boot-shifted\n"};
        for (size_t i = 0; i < 2; ++i) {
            run_program(host_syscall, "/system/bin/art-test", modes[i]);
            REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
            REQUIRE(strstr(benchmark, "android_art_boot_image_loaded"));
            REQUIRE(strstr(benchmark, "android_art_vm_destroyed"));
            fputs(benchmark, stdout);
        }
    } else if (getenv("NCE_ART_OAT_PROBE")) {
        const char *modes[] = {"oat-speed\n", "oat-shifted\n"};
        for (size_t i = 0; i < 2; ++i) {
            run_program(host_syscall, "/system/bin/art-test", modes[i]);
            REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
            REQUIRE(strstr(benchmark, "android_art_aot_entries"));
            REQUIRE(strstr(benchmark, "android_art_vm_destroyed"));
            fputs(benchmark, stdout);
        }
    } else {
        if (!interactive)
            art_regressions(host_syscall);
        if (getenv("NCE_DESKTOP"))
            put_file(host_syscall, "/data/nce/desktop-session", "", 0);
        if (getenv("NCE_SYSTEM_SERVER_PROBE") || getenv("NCE_DESKTOP"))
            put_file(host_syscall, "/data/nce/system-server-probe", "", 0);
        if (getenv("NCE_DIAGNOSTICS"))
            put_file(host_syscall, "/data/nce/diagnostics", "", 0);
        if (getenv("NCE_TRACE_SYSTEMUI"))
            put_file(host_syscall, "/data/nce/trace-systemui", "", 0);
        puts("{\"phase\":\"android_platform_services\"}");
        nce_host_input_start(host_syscall);
        run_program(host_syscall, "/system/bin/android-services-test", NULL);
        nce_host_input_stop();
        REQUIRE(get_file(host_syscall, "/stdout", benchmark, sizeof(benchmark)) > 0);
        if (!interactive || getenv("NCE_DIAGNOSTICS")) {
            REQUIRE(strstr(benchmark, "android_platform_binder_cleanup"));
            REQUIRE(strstr(benchmark, "android_framework_runtime"));
            REQUIRE(strstr(benchmark, "android_vintf_build_consistency"));
            REQUIRE(strstr(benchmark, "android_aconfig_storage"));
            REQUIRE(strstr(benchmark, "android_framework_file_observer"));
            REQUIRE(strstr(benchmark, "android_system_suspend_wakelocks"));
            REQUIRE(strstr(benchmark, "android_installd_app_data"));
            REQUIRE(strstr(benchmark, "android_apex_registry"));
            REQUIRE(strstr(benchmark, (getenv("NCE_SYSTEM_SERVER_PROBE") || getenv("NCE_DESKTOP")) ? "android_system_process_groups_stopped" : "android_zygote_command_server"));
            if (getenv("NCE_SYSTEM_SERVER_PROBE") && !getenv("NCE_DESKTOP")) {
                REQUIRE(strstr(benchmark, "android_system_boot_completed"));
                REQUIRE(strstr(benchmark, "android_desktop_launch_commands"));
                REQUIRE(strstr(benchmark, "android_system_process_groups_stopped"));
            }
            REQUIRE(strstr(benchmark, "android_framework_graphics\",\"passed\":true"));
#ifdef NCE_GRAPHICS
            REQUIRE(strstr(benchmark, "android_gralloc_cpu_gpu_roundtrip\",\"passed\":true"));
#endif
            REQUIRE(strstr(benchmark, "android_crypto"));
        }
#ifdef NCE_GRAPHICS
        nce_graphics_stop();
#endif
        memory_report(host_syscall, "after_framework");
        const char *graphics_path = getenv("NCE_GRAPHICS_OUTPUT");
        if (graphics_path && (!interactive || getenv("NCE_DIAGNOSTICS"))) {
            long bytes = get_file(host_syscall, "/data/nce/framework.png", benchmark, sizeof(benchmark));
            REQUIRE(bytes > 8 && bytes < (long)sizeof(benchmark) - 1);
            REQUIRE(!memcmp(benchmark, "\x89PNG\r\n\x1a\n", 8));
            FILE *graphics = fopen(graphics_path, "wb");
            REQUIRE(graphics && fwrite(benchmark, 1, bytes, graphics) == (size_t)bytes);
            REQUIRE(fclose(graphics) == 0);
            if (getenv("NCE_SYSTEM_SERVER_PROBE") && !getenv("NCE_DESKTOP")) {
                const char *names[] = {"launcher", "clock", "files"};
                size_t capacity = 2 * 1024 * 1024;
                char *pixels = malloc(capacity);
                REQUIRE(pixels);
                for (unsigned i = 0; i < 3; ++i) {
                    char guest[128], host[1024];
                    snprintf(guest, sizeof(guest), "/data/nce/desktop-%s.png", names[i]);
                    REQUIRE(snprintf(host, sizeof(host), "%s.%s.png", graphics_path, names[i]) < (int)sizeof(host));
                    long size = get_file(host_syscall, guest, pixels, capacity);
                    REQUIRE(size > 8 && size < (long)capacity - 1 && !memcmp(pixels, "\x89PNG\r\n\x1a\n", 8));
                    FILE *image = fopen(host, "wb");
                    REQUIRE(image && fwrite(pixels, 1, size, image) == (size_t)size && fclose(image) == 0);
                }
                free(pixels);
            }
        }
    }
    if (!interactive)
        reclaim_regressions(host_syscall);
    long log_args[6] = {10};
    long log_size = host_syscall(116, log_args);
    REQUIRE(log_size > 0);
    char *messages = calloc(1, log_size + 1);
    REQUIRE(messages);
    log_args[0] = 3;
    log_args[1] = (long)messages;
    log_args[2] = log_size;
    REQUIRE(host_syscall(116, log_args) >= 0);
    int warnings = strstr(messages, "WARNING:") || strstr(messages, "BUG:") || strstr(messages, "Bad page");
    if (warnings)
        fputs(messages, stderr);
    free(messages);
    REQUIRE(!warnings);
    puts("{\"test\":\"linux_kernel_warnings\",\"passed\":true}");
}
void nce_process_cleanup(void)
{
    report_mmu_profile();
    atomic_store(&timer_stopping, 1);
    REQUIRE(pthread_join(timer_thread, NULL) == 0);
    REQUIRE(sigaction(SIGUSR2, &old_preempt, NULL) == 0);
    REQUIRE(sigaction(SIGSEGV, &old_segv, NULL) == 0);
    REQUIRE(sigaction(SIGBUS, &old_bus, NULL) == 0);
    REQUIRE(sigaction(SIGILL, &old_ill, NULL) == 0);
    REQUIRE(sigaction(SIGTRAP, &old_trap, NULL) == 0);
    REQUIRE(munmap(ram, ram_size) == 0);
    if (dynamic_cache.source_cache)
        REQUIRE(munmap(dynamic_cache.source_cache, dynamic_cache.size) == 0);
    free(dynamic_cache.cache_prot);
    dynamic_cache = (NceSegment){0};
    /* The virtual address reservation and code belong to memory.c. */
    printf("{\"test\":\"fixed_linux_code_view\",\"copies\":%lu,\"passed\":true}\n", code_copies);
    printf("{\"test\":\"linux_retained_code_bytes\",\"bytes\":%lu}\n", code_reused_bytes);
    printf("{\"test\":\"linux_host_mapping_totals\",\"data_maps\":%lu,\"data_bytes\":%lu,\"code_copy_bytes\":%lu,\"revocations\":%lu}\n",
           data_maps, data_map_bytes, code_copy_bytes, revocations);
    printf("{\"test\":\"linux_nce_context_lifetime\",\"allocated\":%lu,\"freed\":%lu}\n", allocated, freed);
    REQUIRE(allocated == freed);
    printf("{\"test\":\"linux_guest_events\",\"preemptions\":%lu,\"deferred_faults\":%lu,\"stack_resumes\":%lu,\"stack_calls\":%lu}\n", preemptions, deferred_faults, stack_resumes, stack_calls);
    printf("{\"test\":\"guest_preemption_signal_mask\",\"inherited_blocked_threads\":%lu,\"passed\":true}\n", inherited_preempt_blocks);
}
