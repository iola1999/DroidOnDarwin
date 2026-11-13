/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "stock-init-system-observer-data.h"

/* 部署使用原 PID 1 的 namespace；宿主 syscall 任务的 /dev 属于另一根目录。 */
static int stock_init_system_observer_deployed;
static char stock_init_system_observer_boot[37];

struct stock_init_system_observer_stat {
    uint32_t mask, blksize;
    uint64_t attributes;
    uint32_t nlink, uid, gid;
    uint16_t mode, spare;
    uint64_t ino, bytes;
    unsigned char remainder[208];
};
_Static_assert(sizeof(struct stock_init_system_observer_stat) == 256, "Linux statx buffer size");

static void stock_init_system_observer_file(unsigned long scratch, const struct stock_init_system_observer_file *file,
                                            const char *phase, unsigned long exec_index)
{
    int create = !strcmp(phase, "deploy");
    const unsigned long buffer = scratch + 1024;
    unsigned char actual[PAGE - 1024];
    REQUIRE(strlen(file->path) < 256 && strlen(file->selinux) < 256 && file->bytes);
    REQUIRE(copy_user((void *)file->path, scratch, strlen(file->path) + 1, 1) == 0);
    long open_args[6] = {-100, (long)scratch, 0xa0000 | (create ? 0xc2 : 0), file->mode};
    long fd = run_syscall(56, open_args);
    REQUIRE(fd >= 0);
    if (create) {
        long owner[6] = {fd, file->uid, file->gid};
        long mode[6] = {fd, file->mode};
        REQUIRE(run_syscall(55, owner) == 0 && run_syscall(52, mode) == 0);
        for (unsigned long offset = 0; offset < file->bytes;) {
            unsigned long bytes = file->bytes - offset;
            if (bytes > sizeof(actual))
                bytes = sizeof(actual);
            REQUIRE(copy_user((void *)(file->data + offset), buffer, bytes, 1) == 0);
            long write[6] = {fd, (long)buffer, (long)bytes};
            long written = run_syscall(64, write);
            if (written == -4)
                continue;
            REQUIRE(written > 0 && (unsigned long)written <= bytes);
            offset += written;
        }
    }
    REQUIRE(copy_user((void *)"", scratch + 256, 1, 1) == 0);
    long stat[6] = {fd, (long)scratch + 256, 0x1000, 0x21f, (long)buffer};
    struct stock_init_system_observer_stat status = {0};
    REQUIRE(run_syscall(291, stat) == 0 && copy_user(&status, buffer, sizeof(status), 0) == 0);
    REQUIRE((status.mask & 0x21f) == 0x21f && status.mode == (0100000 | file->mode) && status.nlink == 1 &&
            status.uid == file->uid && status.gid == file->gid && status.bytes == file->bytes);
    for (unsigned long offset = 0; offset < file->bytes;) {
        unsigned long bytes = file->bytes - offset;
        if (bytes > sizeof(actual))
            bytes = sizeof(actual);
        long read[6] = {fd, (long)buffer, (long)bytes, (long)offset};
        long count = run_syscall(67, read);
        if (count == -4)
            continue;
        REQUIRE(count > 0 && (unsigned long)count <= bytes && copy_user(actual, buffer, count, 0) == 0);
        REQUIRE(memcmp(actual, file->data + offset, count) == 0);
        offset += count;
    }
    long label_bytes = 0;
    /* 固定 SDK36 的第三次原 init exec 已加载策略；此后读取有效 tmpfs SID 的 context。 */
    if (exec_index >= 3) {
        const char name[] = "security.selinux";
        REQUIRE(copy_user((void *)name, scratch + 256, sizeof(name), 1) == 0);
        long label[6] = {fd, (long)scratch + 256, (long)buffer, sizeof(actual)};
        label_bytes = run_syscall(10, label);
        const long expected_label_bytes = strlen(file->selinux) + 1;
        unsigned long label_copy_bytes = 0;
        int label_copy_result = -1;
        if (label_bytes > 0 && (unsigned long)label_bytes <= sizeof(actual)) {
            label_copy_bytes = label_bytes > 256 ? 256 : label_bytes;
            label_copy_result = copy_user(actual, buffer, label_copy_bytes, 0);
        }
        int label_matches = label_bytes == expected_label_bytes && label_copy_result == 0 &&
                            memcmp(actual, file->selinux, expected_label_bytes) == 0;
        if (!label_matches) {
            flockfile(stdout);
            printf("{\"stock_init_system_observer_label_error\":true,\"phase\":\"%s\",\"exec_index\":%lu,\"pid\":1,\"path\":\"%s\",\"label_bytes\":%ld,\"expected_label_bytes\":%ld,\"label_copy_bytes\":%lu,\"label_copy_result\":%d,\"label_hex\":\"",
                   phase, exec_index, file->path, label_bytes, expected_label_bytes,
                   label_copy_bytes, label_copy_result);
            if (label_copy_result == 0)
                for (unsigned long i = 0; i < label_copy_bytes; ++i)
                    printf("%02x", actual[i]);
            puts("\"}");
            fflush(stdout);
            funlockfile(stdout);
        }
        REQUIRE(label_matches);
    }
    long close[6] = {fd};
    REQUIRE(run_syscall(57, close) == 0);
    flockfile(stdout);
    printf("{\"stock_init_system_observer_file\":true,\"phase\":\"%s\",\"exec_index\":%lu,\"pid\":1,\"path\":\"%s\",\"bytes\":%llu,\"mode\":%u,\"uid\":%u,\"gid\":%u,\"nlink\":%u,\"label_hex\":\"",
           phase, exec_index, file->path, (unsigned long long)status.bytes,
           status.mode, status.uid, status.gid, status.nlink);
    for (long i = 0; i < label_bytes; ++i)
        printf("%02x", actual[i]);
    puts("\",\"content_matches\":true}");
    funlockfile(stdout);
}

static void stock_init_system_observer_check(const char *phase, unsigned long exec_index)
{
    int create = !strcmp(phase, "deploy");
    long mapping[6] = {0, PAGE, 3, 0x22, -1, 0};
    long scratch = run_syscall(222, mapping);
    REQUIRE(scratch > 0);
    const char path[] = "/proc/sys/kernel/random/boot_id";
    REQUIRE(copy_user((void *)path, scratch, sizeof(path), 1) == 0);
    long open[6] = {-100, scratch, 0x80000};
    long fd = run_syscall(56, open);
    REQUIRE(fd >= 0);
    long read[6] = {fd, scratch + 256, 64};
    long count = run_syscall(63, read);
    char boot[38] = {0};
    REQUIRE(count == 37 && copy_user(boot, scratch + 256, count, 0) == 0 && boot[36] == '\n');
    boot[36] = 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            REQUIRE(boot[i] == '-');
        else
            REQUIRE((boot[i] >= '0' && boot[i] <= '9') || (boot[i] >= 'a' && boot[i] <= 'f'));
    }
    long close[6] = {fd};
    REQUIRE(run_syscall(57, close) == 0);
    if (create)
        memcpy(stock_init_system_observer_boot, boot, sizeof(stock_init_system_observer_boot));
    else
        REQUIRE(!strcmp(stock_init_system_observer_boot, boot));
    for (unsigned i = 0; i < STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT; ++i)
        stock_init_system_observer_file(scratch, &stock_init_system_observer_files[i], phase, exec_index);
    long unmap[6] = {scratch, PAGE};
    REQUIRE(run_syscall(215, unmap) == 0);
    flockfile(stdout);
    printf("{\"stock_init_system_observer_ready\":true,\"phase\":\"%s\",\"exec_index\":%lu,\"pid\":1,\"boot_id\":\"%s\",\"files\":%u,\"scratch_unmapped\":true}\n",
           phase, exec_index, boot, (unsigned)STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT);
    funlockfile(stdout);
}

static void stock_init_system_observer_after_syscall(long number, const long *args, long result, unsigned long exec_index,
                                                     unsigned long init_exec_count)
{
    /* /probe-guest 也会挂载 /proc；既有计数确认原 /init 已经执行。 */
    if (!init_exec_count || result || (number != 40 && !(number == 221 && exec_index)))
        return;
    const char *first_stage = getenv("NCE_STOCK_INIT_FIRST_STAGE"), *mode = getenv("NCE_STOCK_INIT_MODE");
    if (!first_stage || strcmp(first_stage, "1") || !mode || strcmp(mode, "original"))
        return;
    long identity[6] = {0};
    if (run_syscall(172, identity) != 1)
        return;
    if (number == 221) {
        if (stock_init_system_observer_deployed)
            stock_init_system_observer_check("exec", exec_index);
        return;
    }
    if (stock_init_system_observer_deployed)
        return;
    unsigned char target[64] = {0}, filesystem[64] = {0};
    if (stock_init_trace_string(target, args[1], sizeof(target)) || strcmp((char *)target, "/proc") ||
        stock_init_trace_string(filesystem, args[2], sizeof(filesystem)) || strcmp((char *)filesystem, "proc"))
        return;
    stock_init_system_observer_deployed = 1;
    stock_init_system_observer_check("deploy", 0);
}
