/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "translation/interaction-profile.h"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
static void *worker(void *value)
{
    NceExecutionProfile *thread = nce_execution_profile_thread(getpid(), (long)value);
    assert(thread);
    for (int i = 0; i < 100; ++i) {
        NceProfileStamp stamp = nce_execution_profile_begin();
        volatile unsigned total = 0;
        for (unsigned j = 0; j < 10000; ++j)
            total += j;
        nce_execution_profile_end(thread, NCE_STAGE_DIRECT, stamp);
    }
    return 0;
}
int main(int argc, char **argv)
{
    assert(argc == 2 && !nce_execution_profile_enabled());
    assert(!nce_execution_profile_begin().wall);
    assert(!nce_execution_profile_control(1, argv[1]));
    pthread_t a, b;
    assert(!pthread_create(&a, 0, worker, (void *)101));
    assert(!pthread_create(&b, 0, worker, (void *)102));
    assert(!pthread_join(a, 0) && !pthread_join(b, 0));
    assert(!nce_execution_profile_control(0, argv[1]));
    assert(!nce_execution_profile_enabled());
    assert(nce_execution_profile_control(1, argv[1]) < 0);
}
