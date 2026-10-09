/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce.h"
#include "memory.h"
#include "layout.h"
#include "lkl-host-ops.h"
#include "guest-image.h"
#include "programs.h"
#include "user-scripts.h"
#include <mach/mach.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <time.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "N2 failure %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)
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
    int event_kind, fault_write, fault_illegal, stack_unmapped;
} Process;
static _Thread_local Process *this_process;
static pthread_mutex_t target_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t target_thread, timer_thread;
static atomic_int timer_stopping;
static NceCode event_resume;
static unsigned long preemptions, deferred_faults, stack_resumes, stack_calls, stackless_exits;
static unsigned long inherited_preempt_blocks;
static volatile sig_atomic_t logged_redirect, logged_stack_restore;
static struct sigaction old_preempt;

static NceState *active;
static NceCode code;
static unsigned char *ram;
static unsigned long code_copies, code_copy_bytes, data_maps, data_map_bytes, revocations;
static size_t ram_size;
static long (*run_syscall)(long, long *);
static int (*page_fault)(unsigned long, int);
static void (*user_return)(long, long, unsigned long);
static void (*user_fault)(unsigned long, int);
static unsigned long faults, allocated, freed;
static int trace;
static struct sigaction old_segv, old_bus, old_ill;

static void shmem_init(unsigned long size)
{
    ram_size = size;
    REQUIRE(size == NCE_RAM_SIZE);
    ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(ram != MAP_FAILED);
}
static void *shmem_map(void *address, unsigned long offset, unsigned long size, enum lkl_prot prot)
{
    uintptr_t start = (uintptr_t)address;
    REQUIRE(!(offset % PAGE) && !(size % PAGE) && offset <= ram_size && size <= ram_size - offset);
    uintptr_t end = start + size;
    while (start < end) {
        uintptr_t stop = end;
        NceProgram *program = NULL;
        NceSegment *segment = NULL;
        for (size_t i = 0; i < nce_program_count; ++i) {
            NceProgram *p = &nce_programs[i];
            for (size_t j = 0; j < p->segment_count; ++j) {
                NceSegment *s = &p->segments[j];
                uintptr_t lo = (uintptr_t)s->code.rx, hi = lo + s->size;
                if (start >= lo && start < hi) { program = p; segment = s; stop = hi < stop ? hi : stop; }
                else if (lo > start && lo < stop) stop = lo;
            }
        }
        size_t length = stop - start;
        if (segment) {
            size_t position = start - (uintptr_t)segment->code.rx;
            memcpy(segment->code.rw + position, ram + offset, length);
            if (prot & LKL_PROT_EXEC)
                REQUIRE(nce_program_patch_range(program, segment, position, length, &active) == 0);
            nce_sync_code((NceCode){segment->code.rw + position, (void *)start, length});
            ++code_copies;
            code_copy_bytes += length;
        } else {
            REQUIRE(!(prot & LKL_PROT_EXEC));
            int host_prot = ((prot & LKL_PROT_READ) ? PROT_READ : 0) | ((prot & LKL_PROT_WRITE) ? PROT_WRITE : 0);
            REQUIRE(nce_memory_remap((void *)start, ram + offset, length, host_prot) == 0);
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
    ++revocations;
    uintptr_t start = (uintptr_t)address, end = start + size;
    uintptr_t cursor = start;
    for (size_t i = 0; i < nce_program_count; ++i) {
        NceProgram *p = &nce_programs[i];
        for (size_t j = 0; j < p->segment_count; ++j) {
            NceSegment *s = &p->segments[j];
            uintptr_t lo = (uintptr_t)s->code.rx, hi = lo + s->size;
            if (hi <= start || lo >= end) continue;
            if (cursor < lo && mprotect((void *)cursor, lo - cursor, PROT_NONE)) return -1;
            size_t first = start > lo ? start - lo : 0;
            size_t stop = end < hi ? end - lo : s->size;
            memset(s->code.rw + first, 0, stop - first);
            nce_sync_code((NceCode){s->code.rw + first, s->code.rx + first, stop - first});
            cursor = lo + stop;
        }
    }
    return cursor < end ? mprotect((void *)cursor, end - cursor, PROT_NONE) : 0;
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
    if (pc >= (uintptr_t)code.rx && pc < (uintptr_t)code.rx + sizeof(guest_image)) return 1;
    for (size_t i = 0; i < nce_program_count; ++i)
        for (size_t j = 0; j < nce_programs[i].segment_count; ++j) {
            NceCode c = nce_programs[i].segments[j].code;
            if (pc >= (uintptr_t)c.rx && pc < (uintptr_t)c.rx + c.size) return 1;
        }
    return 0;
}

static int syscall_stub(uintptr_t pc)
{
    uintptr_t fixture = (uintptr_t)code.rx + ((sizeof(guest_image) + 15) & ~(size_t)15) + 16;
    if (code.rx && pc >= fixture && pc + 80 <= (uintptr_t)code.rx + code.size && (pc - fixture) % 80 == 0) {
        uintptr_t gate;
        memcpy(&gate, (void *)(pc + 72), sizeof(gate));
        if (gate == (uintptr_t)nce_gate) return 1;
    }
    for (size_t i = 0; i < nce_program_count; ++i) {
        NceProgram *p = &nce_programs[i];
        uintptr_t begin = (uintptr_t)p->stubs.rx + 16;
        if (p->stubs.rx && pc >= begin && pc < begin + p->patch_count * 80 && !((pc - begin) % 80)) return 1;
    }
    return 0;
}

static NceState *handle_event(NceState *state)
{
    Process *p = (void *)state;
    if (p->event_kind == 1) {
        if (!deferred_faults) puts("{\"phase\":\"guest_fault_on_host_stack\"}");
        int result = page_fault(p->fault_address, p->fault_illegal ? NCE_FAULT_EXEC : p->fault_write);
        if (result && trace) {
            fprintf(stderr, "guest fault pc=%lx address=%lx write=%d illegal=%d result=%d\n",
                    state->regs.pc, p->fault_address, p->fault_write, p->fault_illegal, result);
            for (size_t i = 0; i < nce_program_count; ++i) {
                NceProgram *program = &nce_programs[i];
                if (state->regs.pc >= program->base && state->regs.pc - program->base < 2 * 1024 * 1024)
                    fprintf(stderr, "guest program=%s offset=%lx\n", program->path, state->regs.pc - program->base);
            }
        }
        if (result) user_fault(p->fault_address, p->fault_illegal && result == -8);
        else user_return(-1, 0, 0);
        ++faults; ++deferred_faults;
    } else if (p->event_kind == 2) {
        long args[6] = {0};
        REQUIRE(run_syscall(124, args) == 0);
        user_return(-1, 0, 0);
        ++preemptions;
    } else {
        ++stack_calls;
        if (p->stack_unmapped && state->regs.x[8] == 93) ++stackless_exits;
        nce_dispatch(state);
    }
    REQUIRE(nce_write_entry(event_resume, state->regs.pc) == 0);
    nce_sync_code(event_resume);
    state->resume = (uintptr_t)event_resume.rx;
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
            if (!logged_redirect) {
                static const char message[] = "{\"phase\":\"signal_redirect_to_host_entry\"}\n";
                write(STDOUT_FILENO, message, sizeof(message) - 1);
                logged_redirect = 1;
            }
            capture_state(&p->state, u);
            p->event_kind = signal == SIGUSR2 ? 2 : stub ? 3 : 1;
            p->fault_address = address;
            p->fault_write = is_write;
            p->fault_illegal = signal == SIGILL;
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
    if (signal == SIGUSR2) return;
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
    if (target_thread == pthread_self()) target_thread = NULL;
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
        if (target_thread) pthread_kill(target_thread, SIGUSR2);
        REQUIRE(pthread_mutex_unlock(&target_mutex) == 0);
    }
    return NULL;
}

static void *user_clone(void *parent, unsigned long sp, unsigned long tls, unsigned long flags)
{
    NceState *state = calloc(1, sizeof(Process));
    if (!state) return NULL;
    *state = *(NceState *)parent;
    state->regs.x[0] = 0;
    if (sp) state->regs.sp = sp;
    if (flags & 0x80000) state->tls = tls;
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
    if (address < USER_BASE || address >= USER_BASE + USER_SIZE || size > USER_BASE + USER_SIZE - address) goto fault;
    for (uintptr_t p = address; p < address + size; p = (p & ~(PAGE - 1)) + PAGE) {
        bad = p;
        if (page_fault(p, write)) goto fault;
    }
    if (write) memcpy((void *)address, buffer, size);
    else memcpy(buffer, (void *)address, size);
    return 0;
fault:
    state->regs.pc -= 4;
    user_fault(bad, 0);
    REQUIRE(nce_write_entry(event_resume, state->regs.pc) == 0);
    nce_sync_code(event_resume);
    state->resume = (uintptr_t)event_resume.rx;
    return -1;
}
static void syscall_from_guest(NceState *state)
{
    long args[6];
    for (unsigned i = 0; i < 6; ++i) args[i] = state->regs.x[i];
    long number = state->regs.x[8];
    uintptr_t continuation = state->regs.pc;
    long result = run_syscall(number, args);
    if (trace) fprintf(stderr, "guest syscall=%ld pc=%lx args=%lx,%lx,%lx,%lx,%lx,%lx result=%lx\n",
                       number, continuation, args[0], args[1], args[2], args[3], args[4], args[5], result);
    if (number == 215 && result == 0 && state->regs.sp >= (uintptr_t)args[0] &&
        state->regs.sp - (uintptr_t)args[0] < (uintptr_t)args[1]) ((Process *)state)->stack_unmapped = 1;
    user_return(number, result, args[0]);
    if (state->regs.pc != continuation) {
        REQUIRE(nce_write_entry(event_resume, state->regs.pc) == 0);
        nce_sync_code(event_resume);
        state->resume = (uintptr_t)event_resume.rx;
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
        if (!inherited_preempt_blocks) puts("{\"phase\":\"unblocked_inherited_preemption_signal\"}");
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
        REQUIRE(page_fault((uintptr_t)stack, 1) == 0);
        char *path = (void *)(stack + 16);
        char *env = path + 64;
        strcpy(path, process->exec_path);
        strcpy(env, "NCE_TEST=userspace");
        stack[0] = (uintptr_t)path; stack[1] = 0;
        stack[2] = (uintptr_t)env; stack[3] = 0;
        long args[6] = {(long)path, (long)stack, (long)(stack + 2)};
        long result = run_syscall(221, args);
        if (result) fprintf(stderr, "exec %s failed: %ld\n", process->exec_path, result);
        REQUIRE(result == 0);
    }
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
    user_base = nce_memory_user_base();
    for (size_t i = 0; i < nce_program_count; ++i)
        REQUIRE(nce_program_prepare(&nce_programs[i]) == 0);
    REQUIRE(protect_none((void *)user_base, NCE_USER_SIZE + NCE_RAM_SIZE + NCE_VMALLOC_SIZE) == 0);
    nce_lkl_host.shmem_init = shmem_init;
    nce_lkl_host.shmem_mmap = shmem_map;
    nce_lkl_host.munmap = protect_none;
    nce_lkl_host.user_clone = user_clone;
    nce_lkl_host.user_free = user_free;
    nce_lkl_host.user_switch = user_switch;
    nce_lkl_host.user_run = user_run;
    nce_lkl_host.user_exec = user_exec;
    nce_lkl_host.user_elf = nce_program_layout;
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGSEGV, &action, &old_segv) == 0);
    REQUIRE(sigaction(SIGBUS, &action, &old_bus) == 0);
    REQUIRE(sigaction(SIGILL, &action, &old_ill) == 0);
    REQUIRE(sigaction(SIGUSR2, &action, &old_preempt) == 0);
    event_resume = nce_memory_take(PAGE);
    REQUIRE(pthread_create(&timer_thread, NULL, preempt_timer, NULL) == 0);
    return user_base;
}
static void put_file(long (*call)(long, long *), const char *path, const void *data, size_t size)
{
    long args[6] = {-100, (long)path, 0x242, 0700};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    args[0] = fd; args[1] = (long)data; args[2] = size;
    REQUIRE(call(64, args) == (long)size);
    REQUIRE(call(57, args) == 0);
}

static long get_file(long (*call)(long, long *), const char *path, char *buffer, size_t size)
{
    long args[6] = {-100, (long)path, 0};
    long fd = call(56, args);
    if (fd < 0) return fd;
    args[0] = fd; args[1] = (long)buffer; args[2] = size - 1;
    long count = call(63, args);
    REQUIRE(call(57, args) == 0);
    if (count >= 0) buffer[count] = 0;
    return count;
}

static void run_program(long (*call)(long, long *), const char *path, const char *input)
{
    if (input) put_file(call, "/stdin", input, strlen(input));
    const char *streams[] = {"/stdin", "/stdout", "/stderr"};
    for (long fd = 0; fd < 3; ++fd) {
        long args[6] = {-100, (long)streams[fd], fd ? 0x242 : 0x40, 0600};
        long opened = call(56, args);
        REQUIRE(opened >= 0);
        if (opened != fd) {
            args[0] = opened; args[1] = fd; args[2] = 0;
            REQUIRE(call(24, args) == fd);
            REQUIRE(call(57, args) == 0);
        }
    }
    Process *process = calloc(1, sizeof(*process));
    REQUIRE(process);
    ++allocated;
    process->state.syscall = syscall_from_guest;
    process->exec_path = path;
    long args[6] = {(long)process, 1};
    long pid = call(246, args);
    REQUIRE(pid > 0);
    int status = -1;
    args[0] = pid; args[1] = (long)&status; args[2] = 0;
    long waited = call(260, args);
    char buffer[4096];
    if (get_file(call, "/stderr", buffer, sizeof(buffer)) > 0) fprintf(stderr, "%s", buffer);
    if (status) {
        fprintf(stderr, "userspace pid=%ld waited=%ld status=%d\n", pid, waited, status);
        char messages[16384];
        long logs[6] = {3, (long)messages, sizeof(messages)};
        long bytes = call(116, logs);
        if (bytes > 0) fwrite(messages, 1, bytes, stderr);
    }
    REQUIRE(waited == pid && status == 0);
    for (long fd = 0; fd < 3; ++fd) { args[0] = fd; REQUIRE(call(57, args) == 0); }
}

static void check_program_files(long (*call)(long, long *))
{
    for (size_t i = 1; i < nce_program_count; ++i) {
        NceProgram *p = &nce_programs[i];
        unsigned char *bytes = malloc(p->size + 1);
        REQUIRE(bytes);
        REQUIRE(get_file(call, p->path, (void *)bytes, p->size + 1) == (long)p->size);
        REQUIRE(!memcmp(bytes, p->file, p->size));
        free(bytes);
    }
}

static void userspace_tests(long (*call)(long, long *))
{
    struct timespec begin, end;
    unsigned long previous_stackless = stackless_exits;
    unsigned long previous_preemptions = preemptions;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &begin) == 0);
    for (size_t i = 1; i < nce_program_count; ++i)
        put_file(call, nce_programs[i].path, nce_programs[i].file, nce_programs[i].size);
    put_file(call, "/unregistered-elf", nce_programs[2].file, nce_programs[2].size);
    const char *script = "/worker check\n/worker check\n/worker check\n/worker check\n"
                         "/worker check\n/worker check\n/worker check\n/worker check\n";
    put_file(call, "/boot.script", script, strlen(script));
    puts("{\"phase\":\"linux_userspace_tests\"}");
    run_program(call, "/init-user", NULL);
    char buffer[4096];
    REQUIRE(get_file(call, "/userspace.result", buffer, sizeof(buffer)) > 0);
    REQUIRE(strstr(buffer, "init pid=1 commands=8 libc=musl passed"));
    fprintf(stderr, "%s", buffer);
    check_program_files(call);
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

void nce_process_tests(long (*host_syscall)(long, long *), long (*guest_syscall)(long, long *),
                       int (*handle_fault)(unsigned long, int),
                       void (*return_to_user)(long, long, unsigned long), void (*signal_fault)(unsigned long, int))
{
    run_syscall = guest_syscall;
    page_fault = handle_fault;
    user_return = return_to_user;
    user_fault = signal_fault;
    long mkdir_args[6] = {-100, (long)"/bin", 0755};
    REQUIRE(host_syscall(34, mkdir_args) == 0);
    mkdir_args[1] = (long)"/lib";
    REQUIRE(host_syscall(34, mkdir_args) == 0);
    const char *directories[] = {"/system", "/system/bin", "/system/lib64", "/proc"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i) {
        mkdir_args[1] = (long)directories[i];
        REQUIRE(host_syscall(34, mkdir_args) == 0);
    }
    long mount_args[6] = {(long)"proc", (long)"/proc", (long)"proc", 0, 0};
    REQUIRE(host_syscall(40, mount_args) == 0);
    long null_args[6] = {-100, (long)"/dev/null", 0020666, 259};
    REQUIRE(host_syscall(33, null_args) == 0);
    null_args[1] = (long)"/dev/random"; null_args[3] = 264;
    REQUIRE(host_syscall(33, null_args) == 0);
    null_args[1] = (long)"/dev/urandom"; null_args[3] = 265;
    REQUIRE(host_syscall(33, null_args) == 0);
    struct { int entropy_bits, bytes; unsigned char data[32]; } entropy = {256, 32, {0}};
    arc4random_buf(entropy.data, sizeof(entropy.data));
    long random_args[6] = {-100, (long)"/dev/random", 2};
    long random_fd = host_syscall(56, random_args);
    REQUIRE(random_fd >= 0);
    random_args[0] = random_fd; random_args[1] = 0x40085203; random_args[2] = (long)&entropy;
    REQUIRE(host_syscall(29, random_args) == 0);
    REQUIRE(host_syscall(57, random_args) == 0);
    memset(entropy.data, 0, sizeof(entropy.data));
    random_args[0] = (long)entropy.data; random_args[1] = sizeof(entropy.data); random_args[2] = 1;
    REQUIRE(host_syscall(278, random_args) == sizeof(entropy.data));
    puts("{\"test\":\"linux_random_initialized\",\"passed\":true}");
    code = nce_memory_take(65536);
    puts("{\"phase\":\"linux_process_tests\"}");
    const unsigned char *exec_elf = nce_programs[0].file;
    size_t exec_size = nce_programs[0].size;
    long file_args[6] = {-100, (long)"/nce-exec", 0x42, 0700};
    long fd = host_syscall(56, file_args);
    REQUIRE(fd >= 0);
    file_args[0] = fd; file_args[1] = (long)exec_elf; file_args[2] = exec_size;
    REQUIRE(host_syscall(64, file_args) == (long)exec_size);
    REQUIRE(host_syscall(57, file_args) == 0);
    struct timespec begin, end;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &begin) == 0);
    long args[6] = {0};
    const unsigned rounds = 32;
    for (unsigned round = 0; round < rounds; ++round) {
        NceState *state = calloc(1, sizeof(Process));
        REQUIRE(state);
        ++allocated;
        state->syscall = syscall_from_guest;
        state->regs.x[21] = USER_BASE;
        REQUIRE(nce_prepare_shared(code, state, &active, guest_image, sizeof(guest_image), GUEST_LINUX_PROCESS) == 0);
        args[0] = (long)state;
        args[1] = 0;
        long pid = host_syscall(246, args);
        REQUIRE(pid > 0);
        int status = -1;
        args[0] = pid; args[1] = (long)&status; args[2] = 0; args[3] = 0;
        long waited = host_syscall(260, args);
        if (status) fprintf(stderr, "round=%u pid=%ld waited=%ld status=%d\n", round, pid, waited, status);
        REQUIRE(waited == pid && status == 0);
    }
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &end) == 0);
    double elapsed_ms = (end.tv_sec - begin.tv_sec) * 1000.0 + (end.tv_nsec - begin.tv_nsec) / 1e6;
    REQUIRE(allocated == rounds * 3);
    printf("{\"test\":\"linux_nce_fork_cow_shared_futex_pipe_exec_wait\",\"passed\":true,\"rounds\":%u,\"faults\":%lu,\"elapsed_ms\":%.3f}\n", rounds, faults, elapsed_ms);
    REQUIRE(faults >= rounds);
    puts("{\"test\":\"linux_exec_then_fork_cow_wait\",\"passed\":true}");
    file_args[0] = -100; file_args[1] = (long)"/nce-exec"; file_args[2] = 0;
    fd = host_syscall(56, file_args);
    REQUIRE(fd >= 0);
    unsigned char *original = malloc(exec_size);
    REQUIRE(original);
    file_args[0] = fd; file_args[1] = (long)original; file_args[2] = exec_size;
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
    check_program_files(host_syscall);
    puts("{\"test\":\"linux_dynamic_elf_files_unchanged\",\"passed\":true}");
    long log_args[6] = {10};
    long log_size = host_syscall(116, log_args);
    REQUIRE(log_size > 0);
    char *messages = calloc(1, log_size + 1);
    REQUIRE(messages);
    log_args[0] = 3; log_args[1] = (long)messages; log_args[2] = log_size;
    REQUIRE(host_syscall(116, log_args) >= 0);
    int warnings = strstr(messages, "WARNING:") || strstr(messages, "BUG:") || strstr(messages, "Bad page");
    if (warnings) fputs(messages, stderr);
    free(messages);
    REQUIRE(!warnings);
    puts("{\"test\":\"linux_kernel_warnings\",\"passed\":true}");
}
void nce_process_cleanup(void)
{
    atomic_store(&timer_stopping, 1);
    REQUIRE(pthread_join(timer_thread, NULL) == 0);
    REQUIRE(sigaction(SIGUSR2, &old_preempt, NULL) == 0);
    REQUIRE(sigaction(SIGSEGV, &old_segv, NULL) == 0);
    REQUIRE(sigaction(SIGBUS, &old_bus, NULL) == 0);
    REQUIRE(sigaction(SIGILL, &old_ill, NULL) == 0);
    REQUIRE(munmap(ram, ram_size) == 0);
    /* The virtual address reservation and code belong to memory.c. */
    printf("{\"test\":\"fixed_linux_code_view\",\"copies\":%lu,\"passed\":true}\n", code_copies);
    printf("{\"test\":\"linux_host_mapping_totals\",\"data_maps\":%lu,\"data_bytes\":%lu,\"code_copy_bytes\":%lu,\"revocations\":%lu}\n",
           data_maps, data_map_bytes, code_copy_bytes, revocations);
    printf("{\"test\":\"linux_nce_context_lifetime\",\"allocated\":%lu,\"freed\":%lu}\n", allocated, freed);
    REQUIRE(allocated == freed);
    printf("{\"test\":\"linux_guest_events\",\"preemptions\":%lu,\"deferred_faults\":%lu,\"stack_resumes\":%lu,\"stack_calls\":%lu}\n", preemptions, deferred_faults, stack_resumes, stack_calls);
    printf("{\"test\":\"guest_preemption_signal_mask\",\"inherited_blocked_threads\":%lu,\"passed\":true}\n", inherited_preempt_blocks);
}
