/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "Bridge.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <unistd.h>

bool nce_debugger_attached(void)
{
    /* CS_DEBUGGED survives detach. P_TRACED reflects the current attachment. */
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc process = {0};
    size_t size = sizeof(process);
    return sysctl(mib, 4, &process, &size, NULL, 0) == 0 &&
           size == sizeof(process) && (process.kp_proc.p_flag & P_TRACED);
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
