/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
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

static void volume_checks(const char *name, const char *node_path)
{
    char path[96];
    CHECK(snprintf(path, sizeof(path), "/stock-init-%s", name) < (int)sizeof(path));
    FILE *input = fopen(path, "r");
    if (!input) {
        CHECK(errno == ENOENT);
        return;
    }
    int held;
    unsigned long long device;
    CHECK(fscanf(input, "%d %llu", &held, &device) == 2 && fclose(input) == 0);
    /* /probe-guest 的 exec 已关闭父任务目录 fd 的 CLOEXEC 副本。 */
    CHECK(held >= 3 && fcntl(held, F_GETFD) == -1 && errno == EBADF);
    CHECK(snprintf(path, sizeof(path), "/%s", name) < (int)sizeof(path));
    held = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat status, node;
    struct statfs filesystem;
    CHECK(held >= 3 && fcntl(held, F_GETFD) == FD_CLOEXEC && fstat(held, &status) == 0);
    CHECK(stat(node_path, &node) == 0 && status.st_dev == node.st_rdev);
    uint64_t actual_device = ((uint64_t)major(status.st_dev) << 32) | minor(status.st_dev);
    CHECK(actual_device == device);
    CHECK(fstatfs(held, &filesystem) == 0 && filesystem.f_type == 0xef53);
    CHECK(snprintf(path, sizeof(path), "/%s/.nce-init-generation", name) < (int)sizeof(path));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    unsigned long long previous = 0;
    char text[128] = {0};
    if (fd >= 0) {
        ssize_t count = read(fd, text, sizeof(text) - 1);
        char extra;
        CHECK(count > 0 && close(fd) == 0 && sscanf(text, "DroidOnDarwin stock-init generation %llu %c", &previous, &extra) == 1);
    } else
        CHECK(errno == ENOENT);
    CHECK(previous < UINT64_MAX);
    int bytes = snprintf(text, sizeof(text), "DroidOnDarwin stock-init generation %llu\n", previous + 1);
    CHECK(bytes > 0 && bytes < (int)sizeof(text));
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, text, bytes) == bytes && fsync(fd) == 0 && close(fd) == 0);
    CHECK(fsync(held) == 0 && close(held) == 0);
    printf("{\"stock_init_%s_generation\":true,\"previous\":%llu,\"generation\":%llu,\"device\":%llu,\"inherited_fd_closed\":true,\"file_fsync\":0,\"directory_fsync\":0}\n", name, previous, previous + 1, (unsigned long long)actual_device);
}

static void original_metadata_checks(const char *profile)
{
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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 1 && !strcmp(argv[0], "/probe-stop-ordinary")) {
        pid_t pid = getpid();
        CHECK(pid > 1 && syscall(247, pid) == -1 && errno == ENOSYS);
        printf("{\"stock_init_ordinary_ready\":true,\"pid\":%d,\"guest_stop_errno\":%d}\n", pid, errno);
        char ready[32];
        int bytes = snprintf(ready, sizeof(ready), "%d\n", pid);
        int fd = open("/stock-init-ordinary-ready", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        CHECK(bytes > 0 && bytes < (int)sizeof(ready) && fd >= 0 && write(fd, ready, bytes) == bytes && close(fd) == 0);
        for (;;)
            pause();
    }
    CHECK(getpid() == 1);
    CHECK(syscall(247, getpid()) == -1 && errno == ENOSYS);
    char profile[32] = {0};
    int fd = open("/stock-init-profile", O_RDONLY);
    CHECK(fd >= 0 && read(fd, profile, sizeof(profile) - 1) > 0 && close(fd) == 0);
    int first_stage = !strcmp(profile, "sdk36-first-stage");
    CHECK(unshare(CLONE_NEWNS) == 0);
    CHECK(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == 0);
    if (first_stage)
        CHECK(umount("/proc") == 0);
    CHECK(mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    char self[32] = {0};
    CHECK(readlink("/proc/self", self, sizeof(self) - 1) > 0 && atoi(self) == 1);
    if (!first_stage)
        original_metadata_checks(profile);
    signal_kernel_restorer_tests();
    if (!first_stage) {
        volume_checks("data", "/dev/block/vdc");
        volume_checks("metadata", "/dev/block/vdd");
    }
    fd = open("/stock-init-mode", O_RDONLY);
    char mode[32] = {0};
    CHECK(fd >= 0 && read(fd, mode, sizeof(mode) - 1) > 0 && close(fd) == 0);
    if (!strcmp(mode, "lifecycle")) {
        int ready[2];
        CHECK(pipe(ready) == 0);
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            CHECK(syscall(247, getpid()) == -1 && errno == ENOSYS);
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
    if (first_stage) {
        CHECK(umount("/proc") == 0);
        printf("{\"stock_init_exec\":true,\"pid\":1,\"path\":\"/init\",\"arguments\":[]}\n");
        const char *args[] = {"/init", NULL};
        execv(args[0], (char *const *)args);
        dprintf(2, "{\"stock_init_exec_errno\":%d}\n", errno);
        return 121;
    }
    printf("{\"stock_init_exec\":true,\"pid\":%d,\"argument\":\"selinux_setup\"}\n", getpid());
    const char *args[] = {"/system/bin/init", "selinux_setup", NULL};
    execv(args[0], (char *const *)args);
    dprintf(2, "{\"stock_init_exec_errno\":%d}\n", errno);
    return 121;
}
