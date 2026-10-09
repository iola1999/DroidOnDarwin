/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "Bridge.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/proc.h>

static enum { DETACHED,
              DELAYED,
              STUCK,
              QUERY_FAILURE,
              SHORT_RESPONSE } scenario;
static unsigned queries;

/* Replace the system query in this host-only executable, preserving its ABI. */
int sysctl(int *name, u_int count, void *old, size_t *size, void *new, size_t new_size)
{
    assert(count == 4 && name[0] == CTL_KERN && name[1] == KERN_PROC && name[2] == KERN_PROC_PID);
    assert(!new && !new_size && *size == sizeof(struct kinfo_proc));
    ++queries;
    if (scenario == QUERY_FAILURE) {
        errno = EIO;
        return -1;
    }
    if (scenario == SHORT_RESPONSE) {
        *size = 0;
        return 0;
    }
    struct kinfo_proc *p = old;
    memset(p, 0, sizeof(*p));
    if (scenario == STUCK || (scenario == DELAYED && queries < 4))
        p->kp_proc.p_flag = P_TRACED;
    return 0;
}

int main(void)
{
    scenario = DETACHED;
    assert(nce_wait_debugger_detach(0) == 0);
    scenario = DELAYED;
    queries = 0;
    assert(nce_wait_debugger_detach(1000) == 0 && queries == 4);
    scenario = STUCK;
    assert(nce_wait_debugger_detach(20) == -2);
    scenario = QUERY_FAILURE;
    assert(nce_wait_debugger_detach(20) == -1);
    scenario = SHORT_RESPONSE;
    assert(nce_wait_debugger_detach(20) == -1);
    puts("detach regression: 5 scenarios passed");
}
