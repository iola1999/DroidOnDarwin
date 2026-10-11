/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "execute-check.h"
#include <setjmp.h>
#include <errno.h>
#include <string.h>
#include <libkern/OSCacheControl.h>
#include <signal.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/ucontext.h>

static struct sigaction previous[3];
static const int signals[] = {SIGBUS, SIGSEGV, SIGILL};
static _Thread_local struct check {
    sigjmp_buf resume;
    uintptr_t start, end;
    volatile uintptr_t pc, address;
    volatile sig_atomic_t signal, running;
} check;

static void caught(int signal, siginfo_t *info, void *opaque)
{
    ucontext_t *context = opaque;
    uintptr_t pc = context->uc_mcontext->__ss.__pc;
    if (check.running && pc >= check.start && pc < check.end) {
        check.signal = signal;
        check.pc = pc;
        check.address = (uintptr_t)info->si_addr;
        siglongjmp(check.resume, 1);
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (signals[i] != signal)
            continue;
        if (previous[i].sa_flags & SA_SIGINFO) {
            previous[i].sa_sigaction(signal, info, opaque);
            return;
        }
        if (previous[i].sa_handler == SIG_IGN)
            return;
        if (previous[i].sa_handler != SIG_DFL) {
            previous[i].sa_handler(signal);
            return;
        }
        sigaction(signal, &previous[i], NULL);
        raise(signal);
        return;
    }
}

int nce_check_execute(NceCode code, int expected)
{
    sigset_t mask;
    if (pthread_sigmask(SIG_BLOCK, NULL, &mask))
        return -1;
    struct sigaction action = {.sa_sigaction = caught, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    unsigned installed = 0;
    for (; installed < 3; ++installed)
        if (sigaction(signals[installed], &action, &previous[installed]))
            break;
    volatile int result = -1;
    check.start = (uintptr_t)code.rx;
    check.end = check.start + code.size;
    check.signal = 0;
    /* Restore only this thread's mask; Darwin siglongjmp with savemask=1
     * uses process-wide sigprocmask and can change unrelated UI threads. */
    if (installed == 3 && sigsetjmp(check.resume, 0) == 0) {
        check.running = 1;
        int (*call)(void) = (void *)code.rx;
        result = call() == expected ? 0 : -1;
    }
    check.running = 0;
    for (unsigned i = 0; i < installed; ++i)
        sigaction(signals[i], &previous[i], NULL);
    if (pthread_sigmask(SIG_SETMASK, &mask, NULL))
        result = -1;
    if (check.signal)
        printf("{\"result\":\"failed\",\"reason\":\"execute_fault\",\"signal\":%d,\"pc\":\"0x%lx\",\"address\":\"0x%lx\"}\n",
               check.signal, (unsigned long)check.pc, (unsigned long)check.address);
    return result;
}

int nce_check_code_updates(NceCode code)
{
    if (!code.rw || !code.rx || code.size < 8 || (uintptr_t)code.rx % 4) {
        errno = EINVAL;
        return -1;
    }
    uint32_t original[2];
    memcpy(original, code.rw, sizeof(original));
    int result = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned value = 42 + i % 2;
        uint32_t words[] = {0x52800000 | (value << 5), 0xd65f03c0};
        memcpy(code.rw, words, sizeof(words));
        sys_dcache_flush(code.rw, sizeof(words));
        sys_icache_invalidate(code.rx, sizeof(words));
        if (nce_check_execute(code, (int)value)) {
            result = -1;
            break;
        }
    }
    memcpy(code.rw, original, sizeof(original));
    sys_dcache_flush(code.rw, sizeof(original));
    sys_icache_invalidate(code.rx, sizeof(original));
    return result;
}
