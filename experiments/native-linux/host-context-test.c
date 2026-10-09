/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lkl-host-ops.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

extern struct lkl_host_operations nce_lkl_host;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "host-context:%d\n", __LINE__); exit(1); } } while (0)
static struct lkl_jmp_buf context;
static int jumped;

static void jump_back(void)
{
    jumped = 1;
    nce_lkl_host.jmp_buf_longjmp(&context, 1);
    abort();
}

static void *worker(void *unused)
{
    (void)unused;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    CHECK(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
    nce_lkl_host.jmp_buf_set(&context, jump_back);
    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0);
    CHECK(sigismember(&mask, SIGUSR2) && jumped);
    return NULL;
}

int main(void)
{
    sigset_t mask, original;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &mask, &original) == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0);
    CHECK(!sigismember(&mask, SIGUSR2));
    CHECK(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    puts("{\"test\":\"host_context_preserves_thread_masks\",\"passed\":true}");
    return 0;
}
