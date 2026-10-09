/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "input.h"
#include <pthread.h>

static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static long (*input_syscall)(long, long *);
static long input_socket = -1;

static long call(long number, long a, long b, long c, long d, long e, long f)
{
    long args[6] = {a, b, c, d, e, f};
    return input_syscall(number, args);
}

void nce_host_input_start(long (*linux_syscall)(long, long *))
{
    pthread_mutex_lock(&input_mutex);
    input_syscall = linux_syscall;
    pthread_mutex_unlock(&input_mutex);
}

void nce_host_input_stop(void)
{
    pthread_mutex_lock(&input_mutex);
    if (input_socket >= 0) call(57, input_socket, 0, 0, 0, 0, 0);
    input_socket = -1;
    input_syscall = 0;
    pthread_mutex_unlock(&input_mutex);
}

int nce_host_input_send(int action, int x, int y)
{
    if (action < NCE_INPUT_DOWN || action > NCE_INPUT_KEY) return -22;
    if (action != NCE_INPUT_KEY && (x < 0 || x >= 360 || y < 0 || y >= 640)) return -22;
    if (action == NCE_INPUT_KEY && x != 102 && x != 158 && x != 580) return -22;
    NceInputCommand command = {action, x, y};
    // Linux sockaddr_un has no Darwin sa_len field.
    struct { uint16_t family; char path[108]; } address = {1, "/dev/nce-input"};
    pthread_mutex_lock(&input_mutex);
    long result = -19;
    if (!input_syscall) goto done;
    if (input_socket < 0) {
        result = call(198, 1, 2 | 0x80000, 0, 0, 0, 0);
        if (result < 0) goto done;
        long opened = result;
        // The test launcher replaces file descriptors 0 through 2.
        input_socket = call(25, opened, 1030, 16, 0, 0, 0);
        call(57, opened, 0, 0, 0, 0, 0);
        if (input_socket < 0) { result = input_socket; goto done; }
    }
    result = call(206, input_socket, (long)&command, sizeof(command), 0x40,
                  (long)&address, sizeof(address));
    if (result >= 0) result = result == sizeof(command) ? 0 : -5;
done:
    pthread_mutex_unlock(&input_mutex);
    return (int)result;
}
