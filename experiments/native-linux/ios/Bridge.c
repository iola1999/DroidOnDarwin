/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "Bridge.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <time.h>
#include <unistd.h>

static int debugger_state(void)
{
    /* CS_DEBUGGED survives detach. P_TRACED reflects the current attachment. */
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc process = {0};
    size_t size = sizeof(process);
    if (sysctl(mib, 4, &process, &size, NULL, 0) != 0) return -1;
    if (size != sizeof(process)) { errno = EPROTO; return -1; }
    return (process.kp_proc.p_flag & P_TRACED) ? 1 : 0;
}
bool nce_debugger_attached(void) { return debugger_state() == 1; }

static uint64_t milliseconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
int nce_wait_debugger_detach(unsigned timeout_ms)
{
    /* debugserver may resume us while arranging its SIGSTOP, before PT_DETACH.
     * Returning from the BRK request therefore precedes detachment completion. */
    uint64_t begin = milliseconds();
    puts("{\"phase\":\"wait_debugger_detach\"}");
    for (;;) {
        int state = debugger_state();
        uint64_t elapsed = milliseconds() - begin;
        if (state == 0) {
            printf("{\"phase\":\"debugger_detached\",\"elapsed_ms\":%llu}\n", (unsigned long long)elapsed);
            return 0;
        }
        if (state < 0) {
            printf("{\"result\":\"failed\",\"reason\":\"debugger_state_query\",\"errno\":%d}\n", errno);
            return -1;
        }
        if (elapsed >= timeout_ms) {
            printf("{\"result\":\"failed\",\"reason\":\"debugger_detach_timeout\",\"ptraced\":%d,\"elapsed_ms\":%llu}\n",
                   state, (unsigned long long)elapsed);
            return -2;
        }
        usleep(10000);
    }
}
int nce_open_log(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return 0;
}
void nce_log(const char *message) { fprintf(stderr, "%s\n", message); }
