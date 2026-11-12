/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

/* Darwin 宿主按 Linux dm-ioctl.h 的固定 ABI 传递原 LP linear 表。 */
struct stock_init_dm_info {
    uint32_t version[3], data_size, data_start, target_count;
    int32_t open_count;
    uint32_t flags, event_nr, padding;
    uint64_t dev;
    char name[128], uuid[129], data[7];
};
struct stock_init_dm_target {
    uint64_t sector_start, length;
    int32_t status;
    uint32_t next;
    char type[16];
};
union stock_init_dm_message {
    struct stock_init_dm_info info;
    unsigned char data[1024];
};
_Static_assert(sizeof(struct stock_init_dm_info) == 312, "Linux dm_ioctl ABI");
_Static_assert(sizeof(struct stock_init_dm_target) == 40, "Linux dm_target_spec ABI");

struct stock_init_partition {
    const char *name, *mount, *filesystem;
    uint64_t offset, size, dev;
    long fd;
    char device[64];
};
static struct stock_init_partition stock_init_partitions[] = {
    {.name = "system", .mount = "/system-image", .filesystem = "ext4"},
    {.name = "product", .mount = "/product", .filesystem = "ext4"},
    {.name = "system_ext", .mount = "/system_ext", .filesystem = "ext4"},
    {.name = "vendor", .mount = "/vendor", .filesystem = "ext4"},
    {.name = "system_dlkm", .mount = "/system_dlkm", .filesystem = "erofs"},
};

static void stock_init_dm_prepare(union stock_init_dm_message *message, const char *name, uint32_t flags)
{
    memset(message, 0, sizeof(*message));
    message->info.version[0] = 4;
    message->info.version[1] = 48;
    message->info.data_size = sizeof(*message);
    message->info.data_start = sizeof(message->info);
    message->info.flags = flags;
    if (name)
        REQUIRE(snprintf(message->info.name, sizeof(message->info.name), "%s", name) < (int)sizeof(message->info.name));
}

static long stock_init_dm_ioctl(long control, unsigned command, union stock_init_dm_message *message)
{
    long args[6] = {control, 0xc138fd00U | command, (long)message};
    return call(29, args);
}

static unsigned stock_init_device_minor(uint64_t dev)
{
    return (dev & 255) | ((dev >> 12) & 0xfff00);
}

static void stock_init_dm_configure(struct stock_init_partition *partition, long control, uint64_t backing)
{
    union stock_init_dm_message message;
    stock_init_dm_prepare(&message, partition->name, 0);
    REQUIRE(stock_init_dm_ioctl(control, 3, &message) == 0); /* DM_DEV_CREATE */
    REQUIRE(message.info.target_count == 0 && message.info.open_count == 0);
    partition->dev = message.info.dev;
    REQUIRE(partition->dev && partition->dev <= UINT32_MAX);
    REQUIRE(snprintf(partition->device, sizeof(partition->device), "/dev/block/dm-%u", stock_init_device_minor(partition->dev)) < (int)sizeof(partition->device));
    long node[6] = {-100, (long)partition->device, 0060000 | 0400, partition->dev};
    REQUIRE(call(33, node) == 0);

    stock_init_dm_prepare(&message, partition->name, 1); /* DM_READONLY_FLAG */
    message.info.target_count = 1;
    struct stock_init_dm_target *target = (void *)(message.data + message.info.data_start);
    target->length = partition->size / 512;
    memcpy(target->type, "linear", sizeof("linear"));
    char *parameters = (void *)(target + 1);
    unsigned major = (backing >> 8) & 0xfff, minor = stock_init_device_minor(backing);
    int count = snprintf(parameters, sizeof(message.data) - message.info.data_start - sizeof(*target), "%u:%u %llu", major, minor, (unsigned long long)(partition->offset / 512));
    REQUIRE(count > 0 && (unsigned)count < sizeof(message.data) - message.info.data_start - sizeof(*target));
    target->next = (sizeof(*target) + count + 1 + 7) & ~7U;
    message.info.data_size = message.info.data_start + target->next;
    REQUIRE(stock_init_dm_ioctl(control, 9, &message) == 0); /* DM_TABLE_LOAD */
    stock_init_dm_prepare(&message, partition->name, 0);
    REQUIRE(stock_init_dm_ioctl(control, 6, &message) == 0); /* DM_DEV_SUSPEND, resume */

    stock_init_dm_prepare(&message, partition->name, 16); /* DM_STATUS_TABLE_FLAG */
    REQUIRE(stock_init_dm_ioctl(control, 12, &message) == 0); /* DM_TABLE_STATUS */
    REQUIRE(message.info.dev == partition->dev && message.info.target_count == 1 &&
            (message.info.flags & 33) == 33 && !(message.info.flags & (2 | 256)) &&
            message.info.data_start >= sizeof(message.info) &&
            message.info.data_size <= sizeof(message) &&
            message.info.data_size >= message.info.data_start + sizeof(*target) + 1);
    target = (void *)(message.data + message.info.data_start);
    parameters = (void *)(target + 1);
    size_t bytes = message.info.data_size - message.info.data_start - sizeof(*target);
    REQUIRE(target->sector_start == 0 && target->length == partition->size / 512 && target->status == 0 &&
            !memcmp(target->type, "linear", sizeof("linear")) && memchr(parameters, 0, bytes));
    unsigned actual_major, actual_minor;
    unsigned long long actual_start;
    char extra;
    REQUIRE(sscanf(parameters, "%u:%u %llu %c", &actual_major, &actual_minor, &actual_start, &extra) == 3 &&
            actual_major == major && actual_minor == minor && actual_start == partition->offset / 512);
    long opened[6] = {-100, (long)partition->device, 0};
    partition->fd = call(56, opened);
    REQUIRE(partition->fd >= 0);
    uint64_t capacity = 0;
    long size[6] = {partition->fd, 0x80081272, (long)&capacity}; /* BLKGETSIZE64 */
    REQUIRE(call(29, size) == 0 && capacity == partition->size);
    long edge[6] = {partition->fd, partition->size - 1, 0};
    REQUIRE(call(62, edge) == (long)partition->size - 1);
    unsigned char byte;
    edge[1] = (long)&byte;
    edge[2] = 1;
    REQUIRE(call(63, edge) == 1);
    long end = call(63, edge);
    REQUIRE(end == 0);
    printf("{\"stock_init_sdk_dm\":true,\"partition\":\"%s\",\"device\":%llu,\"backing_device\":%llu,\"sector_start\":%llu,\"sectors\":%llu,\"backing_start_sector\":%llu,\"capacity\":%llu,\"flags\":%u,\"target\":\"linear\",\"end_read_result\":%ld}\n",
           partition->name, (unsigned long long)partition->dev, (unsigned long long)backing,
           (unsigned long long)target->sector_start, (unsigned long long)target->length,
           actual_start, (unsigned long long)capacity, message.info.flags, end);
}

static void stock_init_sdk_mount(const char *root)
{
    directory("/dev/block");
    long node[6] = {-100, (long)"/dev/device-mapper", 0020000 | 0600, (10 << 8) | 236};
    REQUIRE(call(33, node) == 0);
    long opened[6] = {-100, (long)"/dev/device-mapper", 2};
    long control = call(56, opened);
    REQUIRE(control >= 0);
    union stock_init_dm_message message;
    stock_init_dm_prepare(&message, NULL, 0);
    REQUIRE(stock_init_dm_ioctl(control, 0, &message) == 0 && message.info.version[0] == 4 && message.info.version[1] >= 48);
    printf("{\"stock_init_sdk_dm_version\":[%u,%u,%u]}\n", message.info.version[0], message.info.version[1], message.info.version[2]);
    char sysfs_path[64], sysfs[256] = {0};
    REQUIRE(snprintf(sysfs_path, sizeof(sysfs_path), "/sys/block/ncedisk1/dev") < (int)sizeof(sysfs_path));
    opened[1] = (long)sysfs_path;
    opened[2] = 0;
    long fd = call(56, opened);
    REQUIRE(fd >= 0);
    long read[6] = {fd, (long)sysfs, sizeof(sysfs) - 1};
    REQUIRE(call(63, read) > 0 && call(57, read) == 0);
    unsigned major, minor;
    char extra;
    REQUIRE(sscanf(sysfs, "%u:%u %c", &major, &minor, &extra) == 2 && major < 4096 && minor < (1U << 20));
    uint64_t backing = (major << 8) | (minor & 255) | ((minor & ~255U) << 12);
    opened[1] = (long)"/dev/ncesystem";
    fd = call(56, opened);
    REQUIRE(fd >= 0);
    uint64_t capacity = 0;
    long size[6] = {fd, 0x80081272, (long)&capacity};
    REQUIRE(call(29, size) == 0 && call(57, size) == 0);
    char path[4096], line[256];
    REQUIRE(snprintf(path, sizeof(path), "%s/original-partitions.tsv", root) < (int)sizeof(path));
    FILE *input = fopen(path, "r");
    REQUIRE(input);
    for (unsigned i = 0; i < sizeof(stock_init_partitions) / sizeof(*stock_init_partitions); ++i) {
        struct stock_init_partition *partition = &stock_init_partitions[i];
        char name[32], filesystem[16];
        unsigned long long offset, bytes;
        REQUIRE(fgets(line, sizeof(line), input) && strchr(line, '\n') && sscanf(line, "%31s\t%llu\t%llu\t%15s %c", name, &offset, &bytes, filesystem, &extra) == 4);
        REQUIRE(!strcmp(name, partition->name) && !strcmp(filesystem, partition->filesystem) &&
                offset % 512 == 0 && bytes && bytes % 512 == 0 && offset <= capacity && bytes <= capacity - offset);
        partition->offset = offset;
        partition->size = bytes;
    }
    REQUIRE(!fgets(line, sizeof(line), input) && !ferror(input) && fclose(input) == 0);
    for (unsigned i = 0; i < sizeof(stock_init_partitions) / sizeof(*stock_init_partitions); ++i) {
        struct stock_init_partition *partition = &stock_init_partitions[i];
        stock_init_dm_configure(partition, control, backing);
        directory(partition->mount);
        long mount[6] = {(long)partition->device, (long)partition->mount, (long)partition->filesystem, 1};
        long result = call(40, mount);
        if (result) {
            fprintf(stderr, "stock_init_mount path=%s filesystem=%s result=%ld\n", partition->mount, partition->filesystem, result);
            stock_init_dump_kernel_log();
        }
        REQUIRE(result == 0);
        if (i)
            printf("{\"stock_init_original_partition_mounted\":true,\"path\":\"%s\",\"filesystem\":\"%s\",\"readonly\":true}\n", partition->mount, partition->filesystem);
    }
    opened[0] = control;
    REQUIRE(call(57, opened) == 0);
    opened[0] = -100;
    opened[1] = (long)"/vendor/bin/hw/android.hardware.security.keymint-service";
    opened[2] = 1;
    long file_open = call(56, opened);
    REQUIRE(file_open == -30);
    opened[1] = (long)stock_init_partitions[3].device;
    long block_open = call(56, opened), block_write = block_open;
    if (block_open >= 0) {
        unsigned char byte = 0;
        long write[6] = {block_open, (long)&byte, 1};
        block_write = call(64, write);
        REQUIRE(block_write == -1 && call(57, write) == 0);
    } else
        REQUIRE(block_open == -13);
    printf("{\"stock_init_sdk_readonly\":true,\"file_open_result\":%ld,\"block_open_result\":%ld,\"block_write_result\":%ld}\n", file_open, block_open, block_write);
}

static void stock_init_sdk_cleanup(int lifecycle)
{
    const char *mounts[] = {"/init.environ.rc", "/system", "/vendor", "/product", "/system_ext", "/system", "/system_dlkm", "/vendor", "/system_ext", "/product", "/system-image"};
    unsigned count = sizeof(stock_init_partitions) / sizeof(*stock_init_partitions);
    for (unsigned i = 0; i < sizeof(mounts) / sizeof(*mounts); ++i) {
        long unmount[6] = {(long)mounts[i], 0};
        long result = call(39, unmount);
        if (result)
            fprintf(stderr, "stock_init_unmount path=%s result=%ld\n", mounts[i], result);
        if (!lifecycle && i == 0 && (result == -1 || result == -13)) {
            /* 原策略限制 kernel 域卸载；关闭已持有 fd，再按标准 halt 结束内核。 */
            for (unsigned j = 0; j < count; ++j) {
                long close[6] = {stock_init_partitions[j].fd};
                REQUIRE(call(57, close) == 0);
                stock_init_partitions[j].fd = -1;
            }
            printf("{\"stock_init_sdk_cleanup_deferred\":true,\"path\":\"%s\",\"unmount_result\":%ld,\"closed_partition_fds\":%u}\n", mounts[i], result, count);
            return;
        }
        REQUIRE(result == 0);
    }
    long opened[6] = {-100, (long)"/dev/device-mapper", 2};
    long control = call(56, opened);
    REQUIRE(control >= 0);
    for (unsigned i = count; i-- > 0;) {
        struct stock_init_partition *partition = &stock_init_partitions[i];
        long close[6] = {partition->fd};
        REQUIRE(call(57, close) == 0);
        partition->fd = -1;
        union stock_init_dm_message message;
        long result, waits = 0;
        double deadline = stock_init_now() + 2;
        struct timespec delay = {0, 10000000};
        long sleep[6] = {(long)&delay};
        do {
            stock_init_dm_prepare(&message, partition->name, 0);
            result = stock_init_dm_ioctl(control, 4, &message); /* DM_DEV_REMOVE */
            if (result) {
                REQUIRE(result == -16 && stock_init_now() < deadline);
                REQUIRE(call(101, sleep) == 0);
                ++waits;
            }
        } while (result);
        stock_init_dm_prepare(&message, partition->name, 0);
        long status = stock_init_dm_ioctl(control, 7, &message); /* DM_DEV_STATUS */
        REQUIRE(status == -6);
        long unlink[6] = {-100, (long)partition->device, 0};
        REQUIRE(call(35, unlink) == 0);
        printf("{\"stock_init_sdk_dm_removed\":true,\"partition\":\"%s\",\"device\":%llu,\"remove_result\":%ld,\"status_result\":%ld,\"remove_waits\":%ld}\n", partition->name, (unsigned long long)partition->dev, result, status, waits);
    }
    opened[0] = control;
    REQUIRE(call(57, opened) == 0);
}
