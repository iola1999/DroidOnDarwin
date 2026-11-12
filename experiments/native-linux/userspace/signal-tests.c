/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "signal-tests:%d errno=%d\n", __LINE__, errno); \
            _exit(__LINE__); \
        } \
    } while (0)
static volatile sig_atomic_t received, nested, faulted, alarmed;
static unsigned char *alternate, *guard;
static int bad_frame, redirect;
static pthread_t main_thread;
int signal_register_test(pid_t pid);
uintptr_t signal_redirect_fault(const volatile uint32_t *address);
extern const char signal_redirect_load[], signal_redirect_landing[];
#define REDIRECT_ADDRESS 12

static void handler(int signal, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    unsigned char local;
    CHECK(info->si_signo == signal);
    CHECK((uintptr_t)&local >= (uintptr_t)alternate && (uintptr_t)&local < (uintptr_t)alternate + 65536);
    CHECK(!(uc->uc_mcontext.sp & 15) && !(uc->uc_mcontext.pc & 3));
    struct fpsimd_context *fp = (void *)uc->uc_mcontext.__reserved;
    CHECK(fp->head.magic == FPSIMD_MAGIC && fp->head.size == sizeof(*fp));
    if (signal == SIGUSR1) {
        sigset_t mask;
        CHECK(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR1));
        ++received;
        if (bad_frame == 1)
            uc->uc_mcontext.pc |= 1;
        else if (bad_frame == 2)
            fp->head.magic = 0;
        else if (bad_frame == 3)
            ((struct _aarch64_ctx *)(fp + 1))->size = 16;
        else
            CHECK(raise(SIGUSR2) == 0);
    } else if (signal == SIGUSR2)
        ++nested;
    else if (signal == SIGSEGV && redirect) {
        /* ART's implicit null check returns through a context whose sp is 8
         * below the interrupted sp; Linux restores it without alignment checks. */
        CHECK((uintptr_t)info->si_addr == REDIRECT_ADDRESS && info->si_code == SEGV_MAPERR);
        CHECK(uc->uc_mcontext.fault_address == REDIRECT_ADDRESS);
        CHECK(uc->uc_mcontext.pc == (uintptr_t)signal_redirect_load);
        uc->uc_mcontext.sp -= sizeof(uintptr_t);
        *(uintptr_t *)uc->uc_mcontext.sp = uc->uc_mcontext.pc + 4;
        uc->uc_mcontext.regs[30] = (uintptr_t)info->si_addr;
        uc->uc_mcontext.pc = (uintptr_t)signal_redirect_landing;
        ++faulted;
    } else if (signal == SIGSEGV) {
        CHECK(info->si_addr == guard && info->si_code == SEGV_ACCERR);
        CHECK(mprotect(guard, 16384, PROT_READ | PROT_WRITE) == 0);
        ++faulted;
    } else if (signal == SIGILL) {
        CHECK(info->si_code == ILL_ILLOPC && (uintptr_t)info->si_addr == uc->uc_mcontext.pc);
        uc->uc_mcontext.pc += 4;
        ++faulted;
    } else
        CHECK(0);
}

static void install(int signal, int flags)
{
    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | flags};
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(signal, &action, NULL) == 0);
}

struct send_request {
    int write_fd, delay;
};
static void *send_signal(void *opaque)
{
    struct send_request *request = opaque;
    struct timespec delay = {.tv_nsec = request->delay * 1000000L};
    CHECK(nanosleep(&delay, NULL) == 0);
    CHECK(pthread_kill(main_thread, SIGUSR1) == 0);
    if (request->write_fd >= 0) {
        delay.tv_nsec = 3000000;
        CHECK(nanosleep(&delay, NULL) == 0);
        CHECK(write(request->write_fd, "s", 1) == 1);
    }
    return NULL;
}

static void interrupted_read(int restart)
{
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    install(SIGUSR1, restart ? SA_RESTART : 0);
    struct send_request request = {.write_fd = restart ? pipefd[1] : -1, .delay = 3};
    pthread_t sender;
    CHECK(pthread_create(&sender, NULL, send_signal, &request) == 0);
    char byte;
    ssize_t count = read(pipefd[0], &byte, 1);
    if (restart)
        CHECK(count == 1 && byte == 's');
    else
        CHECK(count == -1 && errno == EINTR);
    CHECK(pthread_join(sender, NULL) == 0);
    CHECK(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
}

#ifndef __BIONIC__
static void *cancel_wait(void *opaque)
{
    int fd = *(int *)opaque;
    char byte;
    CHECK(read(fd, &byte, 1) == 1);
    return NULL;
}
#endif

static void tick(int signal)
{
    CHECK(signal == SIGALRM);
    alarmed = 1;
}

void signal_tests(const char *libc)
{
    main_thread = pthread_self();
    alternate = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(alternate != MAP_FAILED);
    stack_t stack = {.ss_sp = alternate, .ss_size = 65536};
    CHECK(sigaltstack(&stack, NULL) == 0);
    install(SIGUSR1, 0);
    install(SIGUSR2, 0);
    CHECK(raise(SIGUSR1) == 0 && received == 1 && nested == 1);
    sigset_t mask, pending;
    CHECK(sigemptyset(&mask) == 0 && sigaddset(&mask, SIGUSR1) == 0);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    CHECK(raise(SIGUSR1) == 0 && received == 1);
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1));
    CHECK(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0 && received == 2 && nested == 2);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    CHECK(raise(SIGUSR1) == 0);
    struct timespec zero = {0};
    siginfo_t info;
    CHECK(sigtimedwait(&mask, &info, &zero) == SIGUSR1 && info.si_signo == SIGUSR1);
    CHECK(received == 2 && sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0);
    puts("{\"test\":\"linux_signal_mask_altstack_nested\",\"passed\":true}");
    CHECK(signal_register_test(getpid()) == 0);
    puts("{\"test\":\"linux_signal_register_restore\",\"passed\":true}");

    interrupted_read(0);
    interrupted_read(1);
    install(SIGUSR1, 0);
    struct send_request request = {.write_fd = -1, .delay = 3};
    pthread_t sender;
    CHECK(pthread_create(&sender, NULL, send_signal, &request) == 0);
    CHECK(sigemptyset(&mask) == 0);
    CHECK(sigsuspend(&mask) == -1 && errno == EINTR);
    CHECK(pthread_join(sender, NULL) == 0);
    CHECK(pthread_create(&sender, NULL, send_signal, &request) == 0);
    struct timespec duration = {.tv_sec = 1}, remaining;
    CHECK(nanosleep(&duration, &remaining) == -1 && errno == EINTR);
    CHECK(remaining.tv_sec == 0 && remaining.tv_nsec > 0);
    CHECK(pthread_join(sender, NULL) == 0);
    puts("{\"test\":\"linux_signal_eintr_restart\",\"passed\":true}");

    guard = mmap(NULL, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(guard != MAP_FAILED);
    install(SIGSEGV, 0);
    install(SIGILL, 0);
    *(volatile unsigned char *)guard = 42;
    CHECK(guard[0] == 42 && faulted == 1);
    __asm__ volatile(".inst 0");
    CHECK(faulted == 2);
    CHECK(munmap(guard, 16384) == 0);
    puts("{\"test\":\"linux_signal_fault_recovery\",\"passed\":true}");

    redirect = 1;
    CHECK(signal_redirect_fault((void *)REDIRECT_ADDRESS) == REDIRECT_ADDRESS && faulted == 3);
    redirect = 0;
    puts("{\"test\":\"linux_signal_context_redirect\",\"passed\":true}");

    int status;
    for (int invalid = 1; invalid <= 3; ++invalid) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            CHECK(signal(SIGSEGV, SIG_DFL) != SIG_ERR);
            bad_frame = invalid;
            raise(SIGUSR1);
            _exit(99);
        }
        CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    }
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        for (;;)
            __asm__ volatile("" ::: "memory");
    }
    CHECK(kill(child, SIGSTOP) == 0);
    CHECK(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
    CHECK(kill(child, SIGCONT) == 0);
    CHECK(waitpid(child, &status, WCONTINUED) == child && WIFCONTINUED(status));
    CHECK(kill(child, SIGTERM) == 0);
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    puts("{\"test\":\"linux_signal_frame_validation_stop_exit\",\"passed\":true}");

#ifndef __BIONIC__
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    CHECK(pthread_create(&sender, NULL, cancel_wait, &pipefd[0]) == 0);
    CHECK(pthread_cancel(sender) == 0);
    void *result = NULL;
    CHECK(pthread_join(sender, &result) == 0 && result == PTHREAD_CANCELED);
    CHECK(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
    puts("{\"test\":\"linux_pthread_cancel_signal\",\"passed\":true}");
#endif

    CHECK(signal(SIGALRM, tick) != SIG_ERR);
    struct itimerval timer = {.it_value.tv_usec = 5000};
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    while (!alarmed)
        __asm__ volatile("" ::: "memory");
    puts("{\"test\":\"linux_signal_timer_preemption\",\"passed\":true}");
    const int signals[] = {SIGUSR1, SIGUSR2, SIGSEGV, SIGILL, SIGALRM};
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
        CHECK(signal(signals[i], SIG_DFL) != SIG_ERR);
    stack.ss_flags = SS_DISABLE;
    CHECK(sigaltstack(&stack, NULL) == 0);
    CHECK(munmap(alternate, 65536) == 0);
    printf("{\"test\":\"linux_signal_suite\",\"passed\":true,\"libc\":\"%s\"}\n", libc);
}
