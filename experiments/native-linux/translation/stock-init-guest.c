/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "stock-init:%d: %s errno=%d\n", __LINE__, #c, errno); \
            _exit(120); \
        } \
    } while (0)

void signal_kernel_restorer_tests(void);

static void original_metadata_checks(void)
{
    char profile[32] = {0};
    int fd = open("/stock-init-profile", O_RDONLY);
    CHECK(fd >= 0 && read(fd, profile, sizeof(profile) - 1) > 0 && close(fd) == 0);
    if (!strcmp(profile, "sdk36"))
        return;
    CHECK(!strcmp(profile, "hyperos") || !strcmp(profile, "sdk36-lp"));
    FILE *input = fopen("/stock-init-original-metadata.tsv", "r");
    CHECK(input);
    char line[2048];
    while (fgets(line, sizeof(line), input)) {
        unsigned kind, mode, uid, gid;
        char path[1024], expected[513], actual[513], extra;
        unsigned char label[256];
        struct stat status;
        CHECK(strchr(line, '\n') && sscanf(line, "%o\t%o\t%u\t%u\t%1023s\t%512s %c", &kind, &mode, &uid, &gid, path, expected, &extra) == 6);
        CHECK(lstat(path, &status) == 0);
        ssize_t count = lgetxattr(path, "security.selinux", label, sizeof(label));
        if (count <= 0)
            dprintf(2, "stock_init_metadata path=%s uid=%u gid=%u mode=%u label_result=%ld errno=%d\n", path, status.st_uid, status.st_gid, status.st_mode & 07777, (long)count, errno);
        CHECK(count > 0 && count <= (ssize_t)sizeof(label));
        for (ssize_t i = 0; i < count; ++i)
            CHECK(snprintf(actual + i * 2, sizeof(actual) - i * 2, "%02x", label[i]) == 2);
        printf("{\"stock_init_original_metadata\":true,\"path\":\"%s\",\"uid\":%u,\"gid\":%u,\"mode\":%u,\"inode_type\":%u,\"label_hex\":\"%s\"}\n", path, status.st_uid, status.st_gid, status.st_mode & 07777, status.st_mode & S_IFMT, actual);
        CHECK((status.st_mode & S_IFMT) == kind && (status.st_mode & 07777) == mode && status.st_uid == uid && status.st_gid == gid && !strcmp(actual, expected));
    }
    CHECK(!ferror(input) && fclose(input) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(getpid() == 1);
    CHECK(unshare(CLONE_NEWNS) == 0);
    CHECK(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == 0);
    CHECK(mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    char self[32] = {0};
    CHECK(readlink("/proc/self", self, sizeof(self) - 1) > 0 && atoi(self) == 1);
    original_metadata_checks();
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
