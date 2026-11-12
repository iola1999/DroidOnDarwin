/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <time.h>

static void stock_init_setup(void)
{
    const char *root = getenv("NCE_STOCK_INIT_DIRECTORY");
    REQUIRE(root && root[0] == '/');
    const char *original_image = getenv("NCE_STOCK_INIT_SYSTEM");
    int original_system = original_image && original_image[0];
    /* Original first-stage init uses tmpfs so nodes are accessible before restorecon. */
    directory("/dev");
    long devfs[6] = {(long)"tmpfs", (long)"/dev", (long)"tmpfs", 2, (long)"mode=0755"};
    REQUIRE(call(40, devfs) == 0);
    if (original_system) {
        directory("/sys");
        directory("/system-image");
        directory("/system");
        long sysfs[6] = {(long)"sysfs", (long)"/sys", (long)"sysfs"};
        REQUIRE(call(40, sysfs) == 0);
        block_device("/sys/block/ncedisk1/dev", "/dev/ncesystem", 0400);
        long partition[6] = {(long)"/dev/ncesystem", (long)"/system-image", (long)"ext4", 1};
        REQUIRE(call(40, partition) == 0);
        long system[6] = {(long)"/system-image/system", (long)"/system", 0, 4096};
        REQUIRE(call(40, system) == 0);
        puts("{\"stock_init_original_system_mounted\":true,\"readonly\":true}");
    }
    char path[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/files.tsv", root) < (int)sizeof(path));
    FILE *input = fopen(path, "r");
    REQUIRE(input);
    char line[2048];
    while (fgets(line, sizeof(line), input)) {
        char guest[1024], extra;
        unsigned mode;
        REQUIRE(strchr(line, '\n'));
        if (line[0] == 'D') {
            REQUIRE(sscanf(line, "D\t%1023s %c", guest, &extra) == 1);
            if (!original_system || (strcmp(guest, "/system") && strncmp(guest, "/system/", 8)))
                directory(guest);
        } else if (line[0] == 'L') {
            char target[1024], actual[1024] = {0};
            unsigned uid, gid;
            REQUIRE(sscanf(line, "L\t%u\t%u\t%1023s\t%1023s %c", &uid, &gid, guest, target, &extra) == 4);
            REQUIRE(guest[0] == '/' && target[0] == '/' && !strstr(guest, "..") && !strstr(target, ".."));
            long link[6] = {(long)target, -100, (long)guest};
            REQUIRE(call(36, link) == 0);
            long owner[6] = {-100, (long)guest, uid, gid, 0x100};
            REQUIRE(call(54, owner) == 0);
            long readlink[6] = {-100, (long)guest, (long)actual, sizeof(actual) - 1};
            long length = call(78, readlink);
            REQUIRE(length == (long)strlen(target) && !memcmp(actual, target, length));
            printf("{\"stock_init_root_symlink\":true,\"path\":\"%s\",\"target\":\"%s\",\"uid\":%u,\"gid\":%u,\"creation_mode\":511}\n", guest, target, uid, gid);
        } else {
            REQUIRE(sscanf(line, "F\t%o\t%1023s %c", &mode, guest, &extra) == 2);
            REQUIRE(guest[0] == '/' && !strstr(guest, ".."));
            REQUIRE(snprintf(path, sizeof(path), "%s/root%s", root, guest) < (int)sizeof(path));
            if (!original_system || strncmp(guest, "/system/", 8))
                put_host_file(guest, path, mode);
        }
    }
    REQUIRE(!ferror(input) && fclose(input) == 0);
    const char *directories[] = {"/dev", "/dev/socket", "/dev/block", "/dev/pts", "/dev/dm-user", "/sys", "/apex", "/bootstrap-apex", "/linkerconfig", "/mnt", "/data", "/metadata", "/debug_ramdisk", "/second_stage_resources"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        directory(directories[i]);
    const char *devices[] = {"/dev/null", "/dev/random", "/dev/urandom", "/dev/kmsg", "/dev/ptmx"};
    const unsigned numbers[] = {259, 264, 265, 267, 1282};
    for (unsigned i = 0; i < sizeof(devices) / sizeof(*devices); ++i) {
        long args[6] = {-100, (long)devices[i], 0020000 | (i == 3 ? 0600 : 0666), numbers[i]};
        REQUIRE(call(33, args) == 0);
    }
    seed_entropy();
    long console[6] = {8, 0, 8};
    REQUIRE(call(116, console) == 0);
    const char *filesystems[][2] = {{"sysfs", "/sys"}, {"selinuxfs", "/sys/fs/selinux"}, {"devpts", "/dev/pts"}, {"tmpfs", "/apex"}, {"tmpfs", "/bootstrap-apex"}, {"tmpfs", "/linkerconfig"}};
    for (unsigned i = 0; i < sizeof(filesystems) / sizeof(*filesystems); ++i) {
        if (original_system && !strcmp(filesystems[i][0], "sysfs"))
            continue;
        long args[6] = {(long)filesystems[i][0], (long)filesystems[i][1], (long)filesystems[i][0]};
        REQUIRE(call(40, args) == 0);
    }
    const char *partitions[] = {"/system", "/vendor", "/product", "/system_ext"};
    for (unsigned i = 0; i < sizeof(partitions) / sizeof(*partitions); ++i) {
        long args[6] = {(long)partitions[i], (long)partitions[i], 0, 4096};
        REQUIRE(call(40, args) == 0);
    }
    const char *mode = getenv("NCE_STOCK_INIT_MODE");
    REQUIRE(mode && (!strcmp(mode, "lifecycle") || !strcmp(mode, "original")));
    put_text("/stock-init-mode", mode);
}

static double stock_init_now(void)
{
    struct timespec now;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void stock_init_observe_context(long pid, char *previous, long *previous_result)
{
    char path[64], label[256] = {0};
    REQUIRE(snprintf(path, sizeof(path), "/proc/%ld/attr/current", pid) < (int)sizeof(path));
    long args[6] = {-100, (long)path, 0};
    long result = call(56, args);
    if (result >= 0) {
        args[0] = result;
        args[1] = (long)label;
        args[2] = sizeof(label) - 1;
        result = call(63, args);
        REQUIRE(call(57, args) == 0);
    }
    if (result == *previous_result && !strcmp(label, previous))
        return;
    *previous_result = result;
    memcpy(previous, label, sizeof(label));
    printf("{\"stock_init_task_context\":true,\"ancestor_pid\":%ld,\"read_result\":%ld,\"label_hex\":\"", pid, result);
    for (long i = 0; i < result && i < (long)sizeof(label) - 1; ++i)
        printf("%02x", (unsigned char)label[i]);
    puts("\"}");
}

static int stock_init_run(void)
{
    const char *streams[] = {"/stdin", "/stdout", "/stderr"};
    for (long fd = 0; fd < 3; ++fd) {
        long args[6] = {-100, (long)streams[fd], 0x242, 0600};
        long opened = call(56, args);
        REQUIRE(opened >= 0);
        if (opened != fd) {
            long dup[6] = {opened, fd, 0};
            REQUIRE(call(24, dup) == fd && call(57, dup) == 0);
        }
    }
    long launch[6] = {(long)translation_linux_process("/probe-guest"), 1};
    long pid = call(246, launch);
    REQUIRE(pid > 1);
    int status = -1, stopped = 0;
    long waited = 0, killed = 0;
    int lifecycle = !strcmp(getenv("NCE_STOCK_INIT_MODE"), "lifecycle");
    double deadline = stock_init_now() + (lifecycle ? 2 : 30);
    char previous_context[256] = {0};
    long previous_result = -1;
    for (;;) {
        long wait[6] = {pid, (long)&status, 1};
        waited = call(260, wait);
        REQUIRE(waited == 0 || waited == pid);
        if (waited == pid)
            break;
        if (!lifecycle)
            stock_init_observe_context(pid, previous_context, &previous_result);
        if (stock_init_now() >= deadline) {
            long kill[6] = {pid, 9};
            killed = call(129, kill);
            REQUIRE(killed == 0);
            stopped = 1;
            wait[2] = 0;
            waited = call(260, wait);
            REQUIRE(waited == pid);
            break;
        }
        struct timespec delay = {0, 10000000};
        REQUIRE(nanosleep(&delay, NULL) == 0);
    }
    dump_file("/stdout", stdout);
    dump_file("/stderr", stderr);
    char *kernel_log = calloc(1, 1 << 20);
    REQUIRE(kernel_log);
    long log_args[6] = {3, (long)kernel_log, (1 << 20) - 1};
    long log_bytes = call(116, log_args);
    if (log_bytes > 0)
        REQUIRE(fwrite(kernel_log, 1, log_bytes, stderr) == (size_t)log_bytes);
    free(kernel_log);
    printf("{\"stock_init_kernel_log\":true,\"read_result\":%ld}\n", log_bytes);
    printf("{\"stock_init_observation\":true,\"mode\":\"%s\",\"ancestor_pid\":%ld,\"waited\":%ld,\"status\":%d,\"deadline_stop\":%s,\"kill_result\":%ld}\n", getenv("NCE_STOCK_INIT_MODE"), pid, waited, status, stopped ? "true" : "false", killed);
    return lifecycle && (!stopped || status != 9);
}
