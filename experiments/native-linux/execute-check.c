/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "execute-check.h"
#include <setjmp.h>
#include <signal.h>
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
        if (signals[i] != signal) continue;
        if (previous[i].sa_flags & SA_SIGINFO) {
            previous[i].sa_sigaction(signal, info, opaque);
            return;
        }
        if (previous[i].sa_handler == SIG_IGN) return;
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
    struct sigaction action = {.sa_sigaction = caught, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    unsigned installed = 0;
    for (; installed < 3; ++installed)
        if (sigaction(signals[installed], &action, &previous[installed])) break;
    volatile int result = -1;
    check.start = (uintptr_t)code.rx;
    check.end = check.start + code.size;
    check.signal = 0;
    if (installed == 3 && sigsetjmp(check.resume, 1) == 0) {
        check.running = 1;
        int (*call)(void) = (void *)code.rx;
        result = call() == expected ? 0 : -1;
    }
    check.running = 0;
    for (unsigned i = 0; i < installed; ++i) sigaction(signals[i], &previous[i], NULL);
    if (check.signal)
        printf("{\"result\":\"failed\",\"reason\":\"execute_fault\",\"signal\":%d,\"pc\":\"0x%lx\",\"address\":\"0x%lx\"}\n",
               check.signal, (unsigned long)check.pc, (unsigned long)check.address);
    return result;
}
