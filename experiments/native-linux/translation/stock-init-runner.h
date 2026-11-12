/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <time.h>

static void stock_init_setup(void)
{
    const char *root = getenv("NCE_STOCK_INIT_DIRECTORY");
    REQUIRE(root && root[0] == '/');
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
            directory(guest);
        } else {
            REQUIRE(sscanf(line, "F\t%o\t%1023s %c", &mode, guest, &extra) == 2);
            REQUIRE(guest[0] == '/' && !strstr(guest, ".."));
            REQUIRE(snprintf(path, sizeof(path), "%s/root%s", root, guest) < (int)sizeof(path));
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
        long args[6] = {-100, (long)devices[i], 0020666, numbers[i]};
        REQUIRE(call(33, args) == 0);
    }
    seed_entropy();
    long console[6] = {8, 0, 8};
    REQUIRE(call(116, console) == 0);
    const char *filesystems[][2] = {{"sysfs", "/sys"}, {"selinuxfs", "/sys/fs/selinux"}, {"devpts", "/dev/pts"}, {"tmpfs", "/apex"}, {"tmpfs", "/bootstrap-apex"}, {"tmpfs", "/linkerconfig"}};
    for (unsigned i = 0; i < sizeof(filesystems) / sizeof(*filesystems); ++i) {
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
    for (;;) {
        long wait[6] = {pid, (long)&status, 1};
        waited = call(260, wait);
        REQUIRE(waited == 0 || waited == pid);
        if (waited == pid)
            break;
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
