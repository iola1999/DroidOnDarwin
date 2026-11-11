/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            perror(#c); \
            _exit(91); \
        } \
    } while (0)
#ifdef __APPLE__
static int host_pipe2(int descriptors[2], int flags)
{
    if (flags != O_CLOEXEC) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(descriptors))
        return -1;
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    return 0;
}
#define pipe2 host_pipe2
#endif
#include "../../experiments/native-linux/translation/component-supervisor.h"

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    char *environment[] = {NULL};
    struct ComponentResult result = component_run(argv + 1, environment, 1);
    printf("{\"exit_code\":%d,\"signal\":%d,\"exec_errno\":%d,\"usage\":%d,\"timed_out\":%d}\n",
           result.exit_code, result.signal, result.exec_error, result.usage, result.timed_out);
    int status;
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    return 0;
}
