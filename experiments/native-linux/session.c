/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "session.h"
#include <pthread.h>

static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
static long (*session_syscall)(long, long *);

void nce_session_set_syscall(long (*call)(long, long *))
{
    pthread_mutex_lock(&session_mutex);
    session_syscall = call;
    pthread_mutex_unlock(&session_mutex);
}

int nce_session_request_stop(void)
{
    pthread_mutex_lock(&session_mutex);
    long result = -19;
    if (session_syscall) {
        /* /data/nce is temporary for each Linux run. Repeated requests are harmless. */
        long args[6] = {-100, (long)"/data/nce/session-stop", 0x800c1, 0600};
        result = session_syscall(56, args);
        if (result >= 0) {
            args[0] = result;
            result = session_syscall(57, args);
        } else if (result == -17) {
            result = 0;
        }
    }
    pthread_mutex_unlock(&session_mutex);
    return (int)result;
}
