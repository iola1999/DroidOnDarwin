/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "sdk-ramdisk.h"
#include <stddef.h>

struct stock_init_ramdisk_timestamp {
    int64_t seconds;
    uint32_t nanoseconds, reserved;
};
struct stock_init_ramdisk_status {
    uint32_t mask, block_size;
    uint64_t attributes;
    uint32_t links, uid, gid;
    uint16_t mode, reserved;
    uint64_t inode, size, blocks, attribute_mask;
    struct stock_init_ramdisk_timestamp atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    unsigned char remainder[112];
};
_Static_assert(sizeof(struct stock_init_ramdisk_status) == 256, "Linux statx ABI");
_Static_assert(offsetof(struct stock_init_ramdisk_status, mtime) == 112, "Linux statx mtime offset");
_Static_assert(offsetof(struct stock_init_ramdisk_status, rdev_major) == 128, "Linux statx rdev offset");

static void stock_init_ramdisk_attributes(const struct stock_init_ramdisk_entry *entry)
{
    long mode[6] = {-100, (long)entry->path, entry->mode & 07777};
    REQUIRE(call(53, mode) == 0);
    long owner[6] = {-100, (long)entry->path, entry->uid, entry->gid, 0x100};
    REQUIRE(call(54, owner) == 0);
}

static void stock_init_ramdisk_mtime(const struct stock_init_ramdisk_entry *entry)
{
    long times[4] = {entry->mtime, 0, entry->mtime, 0};
    long args[6] = {-100, (long)entry->path, (long)times, 0};
    REQUIRE(call(88, args) == 0);
}

static void stock_init_ramdisk_device(unsigned index)
{
    const struct stock_init_ramdisk_entry *entry = &stock_init_ramdisk_entries[index];
    REQUIRE(entry->rdev_major < 4096 && entry->rdev_minor < (1U << 20));
    uint32_t device = (entry->rdev_major << 8) | (entry->rdev_minor & 255) | ((entry->rdev_minor & ~255U) << 12);
    long make[6] = {-100, (long)entry->path, entry->mode, device};
    long result = call(33, make);
    printf("{\"stock_init_ramdisk_mknod\":%u,\"path\":\"%s\",\"rdev_major\":%u,\"rdev_minor\":%u,\"result\":%ld}\n",
           index + 1, entry->path, entry->rdev_major, entry->rdev_minor, result);
    REQUIRE(result == 0 || result == -17);
    if (result == -17) {
        /* LKL 的默认 rootfs 会创建 /dev/console；归档覆盖前核对设备身份。 */
        struct stock_init_ramdisk_status status = {0};
        long info[6] = {-100, (long)entry->path, 0x100, 0x7ff, (long)&status};
        long observed = call(291, info);
        printf("{\"stock_init_ramdisk_existing_device\":%u,\"path\":\"%s\",\"statx_result\":%ld,\"mask\":%u,\"mode\":%u,\"nlink\":%u,\"rdev_major\":%u,\"rdev_minor\":%u}\n",
               index + 1, entry->path, observed, status.mask, status.mode, status.links, status.rdev_major, status.rdev_minor);
        REQUIRE(observed == 0 && (status.mask & 5) == 5 && (status.mode & 0170000) == 0020000 &&
                status.rdev_major == entry->rdev_major && status.rdev_minor == entry->rdev_minor && status.links == entry->nlink);
    }
}

static void stock_init_ramdisk_write(const struct stock_init_ramdisk_entry *entry)
{
    long open[6] = {-100, (long)entry->path, 0x241, entry->mode & 07777};
    long fd = call(56, open);
    REQUIRE(fd >= 0);
    unsigned long done = 0;
    while (done < entry->bytes) {
        unsigned long bytes = entry->bytes - done;
        if (bytes > 65536)
            bytes = 65536;
        long write[6] = {fd, (long)(entry->data + done), bytes};
        long result = call(64, write);
        if (result == -4)
            continue;
        REQUIRE(result > 0 && (unsigned long)result <= bytes);
        done += result;
    }
    long close[6] = {fd};
    REQUIRE(call(57, close) == 0);
}

static void stock_init_ramdisk_verify(unsigned index)
{
    const struct stock_init_ramdisk_entry *entry = &stock_init_ramdisk_entries[index];
    struct stock_init_ramdisk_status status = {0};
    long info[6] = {-100, (long)entry->path, 0x100, 0x7ff, (long)&status};
    REQUIRE(call(291, info) == 0 && (status.mask & 0x25f) == 0x25f);
    REQUIRE(status.mode == entry->mode && status.uid == entry->uid && status.gid == entry->gid &&
            status.mtime.seconds == entry->mtime && status.mtime.nanoseconds == 0);
    unsigned kind = entry->mode & 0170000;
    if (kind != 0040000)
        REQUIRE(status.links == entry->nlink);
    REQUIRE(status.rdev_major == entry->rdev_major && status.rdev_minor == entry->rdev_minor);
    if (kind == 0100000) {
        REQUIRE(status.size == entry->bytes);
        long open[6] = {-100, (long)entry->path, 0};
        long fd = call(56, open);
        REQUIRE(fd >= 0);
        unsigned char buffer[65536];
        for (unsigned long offset = 0; offset < entry->bytes;) {
            unsigned long bytes = entry->bytes - offset;
            if (bytes > sizeof(buffer))
                bytes = sizeof(buffer);
            long read[6] = {fd, (long)buffer, bytes, offset};
            long result = call(67, read);
            if (result == -4)
                continue;
            REQUIRE(result == (long)bytes && !memcmp(buffer, entry->data + offset, bytes));
            offset += bytes;
        }
        long end[6] = {fd, (long)buffer, 1, entry->bytes};
        REQUIRE(call(67, end) == 0 && call(57, end) == 0);
    }
    printf("{\"stock_init_ramdisk_entry\":%u,\"archive\":%u,\"path\":\"%s\",\"mode\":%u,\"uid\":%u,\"gid\":%u,\"nlink\":%u,\"size\":%llu,\"mtime_sec\":%lld,\"mtime_nsec\":%u,\"rdev_major\":%u,\"rdev_minor\":%u,\"content_matches\":true}\n",
           index + 1, entry->archive, entry->path, status.mode, status.uid, status.gid, status.links,
           (unsigned long long)status.size, (long long)status.mtime.seconds, status.mtime.nanoseconds,
           status.rdev_major, status.rdev_minor);
}

static void stock_init_ramdisk_setup(void)
{
    char cmdline[4096] = {0};
    long open[6] = {-100, (long)"/proc/cmdline", 0};
    long fd = call(56, open);
    REQUIRE(fd >= 0);
    long read[6] = {fd, (long)cmdline, sizeof(cmdline) - 1};
    long bytes = call(63, read);
    REQUIRE(bytes > 0 && bytes < (long)sizeof(cmdline) && call(57, read) == 0);
    flockfile(stdout);
    printf("{\"stock_init_first_stage_cmdline\":true,\"value_hex\":\"");
    for (long i = 0; i < bytes; ++i)
        printf("%02x", (unsigned char)cmdline[i]);
    puts("\"}");
    funlockfile(stdout);
    long unmount[6] = {(long)"/proc", 0};
    REQUIRE(call(39, unmount) == 0);
    unsigned count = sizeof(stock_init_ramdisk_entries) / sizeof(*stock_init_ramdisk_entries);
    for (unsigned i = 0; i < count; ++i) {
        const struct stock_init_ramdisk_entry *entry = &stock_init_ramdisk_entries[i];
        unsigned kind = entry->mode & 0170000;
        if (kind == 0040000) {
            long make[6] = {-100, (long)entry->path, entry->mode & 07777};
            long result = call(34, make);
            REQUIRE(result == 0 || result == -17);
        } else if (kind == 0020000) {
            stock_init_ramdisk_device(i);
        } else {
            REQUIRE(kind == 0100000);
            stock_init_ramdisk_write(entry);
        }
        stock_init_ramdisk_attributes(entry);
        if (kind != 0040000)
            stock_init_ramdisk_mtime(entry);
    }
    /* 子项创建会更新目录 mtime，全部部署后按原 stream 顺序恢复。 */
    for (unsigned i = 0; i < count; ++i)
        if ((stock_init_ramdisk_entries[i].mode & 0170000) == 0040000)
            stock_init_ramdisk_mtime(&stock_init_ramdisk_entries[i]);
    for (unsigned i = 0; i < count; ++i)
        stock_init_ramdisk_verify(i);
    seed_entropy();
    long mount[6] = {(long)"proc", (long)"/proc", (long)"proc", 2 | 4 | 8};
    REQUIRE(call(40, mount) == 0);
}
