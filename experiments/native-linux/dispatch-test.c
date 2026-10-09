/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int nce_lkl_run(const char *path);

static void run_probe(void *path)
{
    sigset_t mask;
    if (pthread_sigmask(SIG_BLOCK, NULL, &mask))
        exit(2);
    printf("{\"test\":\"dispatch_signal_mask\",\"sigusr2_blocked\":%d}\n", sigismember(&mask, SIGUSR2));
    fflush(stdout);
    exit(nce_lkl_run(path));
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    /* Match the iOS application's entry through a GCD worker. */
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), argv[1], run_probe);
    dispatch_main();
}
