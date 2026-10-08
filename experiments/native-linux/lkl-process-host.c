/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce.h"
#include "memory.h"
#include "layout.h"
#include "lkl-host-ops.h"
#include "guest-image.h"
#include "exec-image.h"
#include <mach/mach.h>
#include <signal.h>
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
typedef struct { NceState state; void *signal_stack; } Process;
static NceState *active;
static NceCode code;
static unsigned char *ram;
static NceCode executable;
static unsigned long code_backing[NCE_EXEC_SIZE / NCE_PAGE_SIZE];
static unsigned long code_copies;
static size_t ram_size;
static long (*run_syscall)(long, long *);
static int (*page_fault)(unsigned long, int);
static unsigned long faults, allocated, freed;
static struct sigaction old_segv, old_bus;

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
    uintptr_t code_start = (uintptr_t)executable.rx;
    REQUIRE(!(offset % PAGE) && !(size % PAGE) && offset <= ram_size && size <= ram_size - offset);
    if (start >= code_start && start < code_start + executable.size) {
        REQUIRE(size <= code_start + executable.size - start);
        size_t position = start - code_start;
        /* Execute from the debugger-prepared address. Task switches copy
         * Linux's private pages without replacing this VM entry. */
        memcpy(executable.rw + position, ram + offset, size);
        for (size_t i = 0; i < size; i += PAGE) code_backing[(position + i) / PAGE] = offset + i;
        nce_sync_code((NceCode){executable.rw + position, executable.rx + position, size});
        ++code_copies;
        return address;
    }
    REQUIRE(!(prot & LKL_PROT_EXEC));
    int host_prot = ((prot & LKL_PROT_READ) ? PROT_READ : 0) | ((prot & LKL_PROT_WRITE) ? PROT_WRITE : 0);
    REQUIRE(nce_memory_remap(address, ram + offset, size, host_prot) == 0);
    return address;
}
static int protect_none(void *address, unsigned long size)
{
    uintptr_t start = (uintptr_t)address, end = start + size;
    uintptr_t code_start = (uintptr_t)executable.rx, code_end = code_start + executable.size;
    if (end <= code_start || start >= code_end) return mprotect(address, size, PROT_NONE);
    if (start < code_start && mprotect((void *)start, code_start - start, PROT_NONE)) return -1;
    if (end > code_end && mprotect((void *)code_end, end - code_end, PROT_NONE)) return -1;
    size_t offset = start > code_start ? start - code_start : 0;
    size_t stop = end < code_end ? end - code_start : executable.size;
    /* UDF instructions reject stale execution while the RX mapping stays put. */
    memset(executable.rw + offset, 0, stop - offset);
    for (size_t i = offset; i < stop; i += PAGE) code_backing[i / PAGE] = ~0UL;
    nce_sync_code((NceCode){executable.rw + offset, executable.rx + offset, stop - offset});
    return 0;
}

static void fault(int signal, siginfo_t *info, void *opaque)
{
    ucontext_t *context = opaque;
    uintptr_t address = (uintptr_t)info->si_addr;
    int write = (context->uc_mcontext->__es.__esr & (1u << 6)) != 0;
    if (address >= USER_BASE && address < USER_BASE + USER_SIZE && page_fault &&
        page_fault(address, write) == 0) {
        ++faults;
        return;
    }
    fprintf(stderr, "N2 fatal signal=%d address=%p pc=%llx esr=%x active=%p\n",
            signal, info->si_addr, context->uc_mcontext->__ss.__pc,
            context->uc_mcontext->__es.__esr, (void *)active);
    _Exit(128 + signal);
}
static void user_switch(void *state) { active = state; }
static void *user_clone(void *parent, unsigned long sp, unsigned long tls, unsigned long flags)
{
    NceState *state = calloc(1, sizeof(Process));
    if (!state) return NULL;
    *state = *(NceState *)parent;
    state->x[0] = 0;
    if (sp) state->sp = sp;
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
static void syscall_from_guest(NceState *state)
{
    long args[6];
    for (unsigned i = 0; i < 6; ++i) args[i] = state->x[i];
    state->x[0] = run_syscall(state->x[8], args);
}
static void user_run(void *opaque)
{
    NceState *state = opaque;
    Process *process = opaque;
    process->signal_stack = malloc(65536);
    REQUIRE(process->signal_stack);
    stack_t signal_stack = {.ss_sp = process->signal_stack, .ss_size = 65536};
    REQUIRE(sigaltstack(&signal_stack, NULL) == 0);
    if (!state->sp) {
        long args[6] = {USER_BASE + 0xf00000, 65536, 3, 0x32, -1, 0};
        REQUIRE(run_syscall(222, args) == args[0]);
        state->sp = args[0] + args[1];
    }
    active = state;
    nce_enter(state);
    REQUIRE(0); /* Linux exit terminates the task without returning to NCE. */
}
static void user_exec(void *opaque, unsigned long entry, unsigned long stack)
{
    Process *process = opaque;
    NceState *state = &process->state;
    unsigned char original[EXEC_TEXT_SIZE];
    /* start_thread() made these pages private with FOLL_WRITE | FOLL_FORCE. */
    REQUIRE(entry == (uintptr_t)executable.rx);
    memcpy(original, (void *)entry, sizeof(original));
    memset(state->x, 0, sizeof(state->x));
    memset(state->vector, 0, sizeof(state->vector));
    state->sp = stack;
    state->tls = state->nzcv = state->fpcr = state->fpsr = 0;
    REQUIRE(nce_prepare_shared(executable, state,
                               &active, original, sizeof(original), 0) == 0);
    /* Preserve patched instructions in Linux's private backing pages so the
     * next switch can restore this process without changing its code address. */
    for (size_t i = 0; i < executable.size; i += PAGE) {
        REQUIRE(code_backing[i / PAGE] < ram_size);
        memcpy(ram + code_backing[i / PAGE], executable.rw + i, PAGE);
    }
}
unsigned long nce_process_setup(void)
{
    REQUIRE(getpagesize() == PAGE);
    executable = nce_memory_guest_code();
    user_base = (uintptr_t)executable.rx - NCE_EXEC_OFFSET;
    memset(code_backing, 0xff, sizeof(code_backing));
    REQUIRE(protect_none((void *)user_base, NCE_USER_SIZE + NCE_RAM_SIZE + NCE_VMALLOC_SIZE) == 0);
    nce_lkl_host.shmem_init = shmem_init;
    nce_lkl_host.shmem_mmap = shmem_map;
    nce_lkl_host.munmap = protect_none;
    nce_lkl_host.user_clone = user_clone;
    nce_lkl_host.user_free = user_free;
    nce_lkl_host.user_switch = user_switch;
    nce_lkl_host.user_run = user_run;
    nce_lkl_host.user_exec = user_exec;
    struct sigaction action = {.sa_sigaction = fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGSEGV, &action, &old_segv) == 0);
    REQUIRE(sigaction(SIGBUS, &action, &old_bus) == 0);
    return user_base;
}
void nce_process_tests(long (*host_syscall)(long, long *), long (*guest_syscall)(long, long *),
                       int (*handle_fault)(unsigned long, int))
{
    run_syscall = guest_syscall;
    page_fault = handle_fault;
    code = nce_memory_take(65536);
    puts("{\"phase\":\"linux_process_tests\"}");
    long file_args[6] = {-100, (long)"/nce-exec", 0x42, 0700};
    long fd = host_syscall(56, file_args);
    REQUIRE(fd >= 0);
    file_args[0] = fd; file_args[1] = (long)exec_elf; file_args[2] = sizeof(exec_elf);
    REQUIRE(host_syscall(64, file_args) == sizeof(exec_elf));
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
        state->x[21] = USER_BASE;
        REQUIRE(nce_prepare_shared(code, state, &active, guest_image, sizeof(guest_image), GUEST_LINUX_PROCESS) == 0);
        args[0] = (long)state;
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
    unsigned char *original = malloc(sizeof(exec_elf));
    REQUIRE(original);
    file_args[0] = fd; file_args[1] = (long)original; file_args[2] = sizeof(exec_elf);
    REQUIRE(host_syscall(63, file_args) == sizeof(exec_elf));
    REQUIRE(memcmp(original, exec_elf, sizeof(exec_elf)) == 0);
    free(original);
    REQUIRE(host_syscall(57, file_args) == 0);
    puts("{\"test\":\"linux_exec_file_unchanged\",\"passed\":true}");
    REQUIRE(host_syscall(-1, args) == -38 && host_syscall(99999, args) == -38);
    puts("{\"test\":\"linux_syscall_bounds\",\"passed\":true}");
}
void nce_process_cleanup(void)
{
    REQUIRE(sigaction(SIGSEGV, &old_segv, NULL) == 0);
    REQUIRE(sigaction(SIGBUS, &old_bus, NULL) == 0);
    REQUIRE(munmap(ram, ram_size) == 0);
    /* The virtual address reservation and code belong to memory.c. */
    printf("{\"test\":\"fixed_linux_code_view\",\"copies\":%lu,\"passed\":true}\n", code_copies);
    printf("{\"test\":\"linux_nce_context_lifetime\",\"allocated\":%lu,\"freed\":%lu}\n", allocated, freed);
    REQUIRE(allocated == freed);
}
