/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Single-CPU component diagnostic. Events use the host syscall boundary,
 * independent of delayed stdout forwarding by the component supervisor. */
static bool observe_integer;
static unsigned long observation_sequence;
static unsigned long observation_mm;
static unsigned long observation_vm_pending;

static void observe_setup(const char *workload)
{
    const char *option = getenv("NCE_STOCK_ART_OBSERVE_JIT");
    REQUIRE(!option || !strcmp(option, "0") || !strcmp(option, "1"));
    observe_integer = option && !strcmp(option, "1");
#ifndef NCE_DIRECT_EXECUTION
    REQUIRE(!observe_integer);
#endif
    REQUIRE(!observe_integer || (execution_count == 1 && translation && workload && !strcmp(workload, "stock-art")));
}

static void observe_identity(Process *p)
{
    if (observe_integer && !p->observed_pid) {
        long args[6] = {0};
        p->observed_pid = run_syscall(172, args);
        p->observed_tid = run_syscall(178, args);
        REQUIRE(p->observed_pid > 0 && p->observed_tid > 0);
    }
}

#ifdef NCE_DIRECT_EXECUTION
static void observe_pc(Process *p, uint64_t completed, uint64_t next)
{
    if (observe_integer && p->observed_window && completed != UINT64_MAX)
        printf("{\"execution_observation\":\"integer_completed\",\"sequence\":%lu,\"pid\":%ld,\"tid\":%ld,\"mm\":%lu,\"pc\":%llu,\"next_pc\":%llu}\n",
               ++observation_sequence, p->observed_pid, p->observed_tid, execution->view->pgd,
               (unsigned long long)completed, (unsigned long long)next);
}
#endif

static bool observe_vm(Process *p, long number, const long *args)
{
    /* Logging before dispatch also covers a call which blocks or never returns.
     * Failed overlapping requests conservatively invalidate a candidate. */
    if (!observe_integer || !(number == 222 || number == 226 || number == 216 || number == 215 || number == 214 || number == 221 || number == 196 || number == 197 || number == 233 || number == 281 || number == 282 || number == 234 || number == 288 || number == 440))
        return false;
    /* A VM call started before begin can resume inside the window. */
    ++observation_vm_pending;
    if (observation_mm && (observation_mm == execution->view->pgd || number == 440))
        printf("{\"execution_observation\":\"vm_call\",\"sequence\":%lu,\"pid\":%ld,\"tid\":%ld,\"mm\":%lu,\"number\":%ld,\"args\":[%lu,%lu,%lu,%lu,%lu,%lu]}\n",
               ++observation_sequence, p->observed_pid, p->observed_tid, execution->view->pgd, number,
               (unsigned long)args[0], (unsigned long)args[1], (unsigned long)args[2],
               (unsigned long)args[3], (unsigned long)args[4], (unsigned long)args[5]);
    return true;
}

static void observe_marker(Process *p, long number, const long *args, long result)
{
    if (!observe_integer || number != 167 || args[0] != 15 || result)
        return;
    char name[16];
    if (copy_user(name, args[1], sizeof(name), 0) || !memchr(name, 0, sizeof(name)))
        return;
    if (strcmp(name, "art-jit-begin") && strcmp(name, "art-jit-end"))
        return;
    p->observed_window = !strcmp(name, "art-jit-begin");
    observation_mm = p->observed_window ? execution->view->pgd : 0;
    printf("{\"execution_observation\":\"marker\",\"sequence\":%lu,\"pid\":%ld,\"tid\":%ld,\"mm\":%lu,\"name\":\"%s\",\"pending_vm_calls\":%lu}\n",
           ++observation_sequence, p->observed_pid, p->observed_tid, execution->view->pgd, name, observation_vm_pending);
}
