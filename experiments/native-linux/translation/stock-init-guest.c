/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "stock-init:%d: %s errno=%d\n", __LINE__, #c, errno); \
            _exit(120); \
        } \
    } while (0)

void signal_kernel_restorer_tests(void);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(getpid() == 1);
    CHECK(unshare(CLONE_NEWNS) == 0);
    CHECK(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == 0);
    CHECK(mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    char self[32] = {0};
    CHECK(readlink("/proc/self", self, sizeof(self) - 1) > 0 && atoi(self) == 1);
    signal_kernel_restorer_tests();
    int fd = open("/stock-init-mode", O_RDONLY);
    char mode[32] = {0};
    CHECK(fd >= 0 && read(fd, mode, sizeof(mode) - 1) > 0 && close(fd) == 0);
    if (!strcmp(mode, "lifecycle")) {
        int ready[2];
        CHECK(pipe(ready) == 0);
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            signal_kernel_restorer_tests();
            CHECK(close(ready[0]) == 0 && write(ready[1], "r", 1) == 1 && close(ready[1]) == 0);
            for (;;)
                pause();
        }
        char value;
        CHECK(close(ready[1]) == 0 && read(ready[0], &value, 1) == 1 && value == 'r' && close(ready[0]) == 0);
        printf("{\"stock_init_lifecycle\":true,\"pid\":%d,\"child\":%d}\n", getpid(), child);
        for (;;)
            pause();
    }
    CHECK(!strcmp(mode, "original"));
    printf("{\"stock_init_exec\":true,\"pid\":%d,\"argument\":\"selinux_setup\"}\n", getpid());
    const char *args[] = {"/system/bin/init", "selinux_setup", NULL};
    execv(args[0], (char *const *)args);
    dprintf(2, "{\"stock_init_exec_errno\":%d}\n", errno);
    return 121;
}
