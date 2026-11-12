/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

/* Darwin 宿主按 Linux include/uapi/linux/loop.h 的固定 ABI 传递 ioctl 数据。 */
struct stock_init_loop_info {
    uint64_t device, inode, rdevice, offset, sizelimit;
    uint32_t number, encrypt_type, encrypt_key_size, flags;
    unsigned char file_name[64], crypt_name[64], encrypt_key[32];
    uint64_t init[2];
};
struct stock_init_loop_config {
    uint32_t fd, block_size;
    struct stock_init_loop_info info;
    uint64_t reserved[8];
};
_Static_assert(sizeof(struct stock_init_loop_info) == 232, "Linux loop_info64 ABI");
_Static_assert(sizeof(struct stock_init_loop_config) == 304, "Linux loop_config ABI");

struct stock_init_loop {
    const char *name, *mount, *filesystem;
    uint64_t offset, size;
    long fd, id;
    char device[64];
};
static struct stock_init_loop stock_init_loops[] = {
    {.name = "system", .mount = "/system-image", .filesystem = "ext4"},
    {.name = "product", .mount = "/product", .filesystem = "ext4"},
    {.name = "system_ext", .mount = "/system_ext", .filesystem = "ext4"},
    {.name = "vendor", .mount = "/vendor", .filesystem = "ext4"},
    {.name = "system_dlkm", .mount = "/system_dlkm", .filesystem = "erofs"},
};

static void stock_init_loop_configure(struct stock_init_loop *loop, long control, long backing, int temporary)
{
    long request[6] = {control, 0x4c82}; /* LOOP_CTL_GET_FREE */
    loop->id = call(29, request);
    REQUIRE(loop->id >= 0 && loop->id < 256);
    char sysfs[64];
    REQUIRE(snprintf(sysfs, sizeof(sysfs), "/sys/block/loop%ld/dev", loop->id) < (int)sizeof(sysfs));
    REQUIRE(snprintf(loop->device, sizeof(loop->device), "/dev/block/loop%ld", loop->id) < (int)sizeof(loop->device));
    block_device(sysfs, loop->device, 0400);
    long opened[6] = {-100, (long)loop->device, 0};
    loop->fd = call(56, opened);
    REQUIRE(loop->fd >= 0);
    struct stock_init_loop_config config = {.fd = backing, .info = {.offset = loop->offset, .sizelimit = loop->size, .flags = 1}};
    REQUIRE(snprintf((char *)config.info.file_name, sizeof(config.info.file_name), "sdk36.%s", loop->name) < (int)sizeof(config.info.file_name));
    request[0] = loop->fd;
    request[1] = 0x4c0a; /* LOOP_CONFIGURE */
    request[2] = (long)&config;
    REQUIRE(call(29, request) == 0);
    struct stock_init_loop_info actual = {0};
    request[1] = 0x4c05; /* LOOP_GET_STATUS64 */
    request[2] = (long)&actual;
    REQUIRE(call(29, request) == 0 && actual.offset == loop->offset && actual.sizelimit == loop->size && actual.flags == 1 && actual.number == loop->id);
    uint64_t capacity = 0;
    request[1] = 0x80081272; /* BLKGETSIZE64 */
    request[2] = (long)&capacity;
    REQUIRE(call(29, request) == 0 && capacity == loop->size);
    long edge[6] = {loop->fd, loop->size - 1, 0};
    REQUIRE(call(62, edge) == (long)loop->size - 1);
    unsigned char byte;
    edge[1] = (long)&byte;
    edge[2] = 1;
    REQUIRE(call(63, edge) == 1);
    long end_read = call(63, edge);
    REQUIRE(end_read == 0);
    printf("{\"stock_init_sdk_loop\":true,\"partition\":\"%s\",\"loop_id\":%ld,\"offset\":%llu,\"sizelimit\":%llu,\"capacity\":%llu,\"flags\":%u,\"end_read_result\":%ld,\"temporary\":%s}\n",
           loop->name, loop->id, (unsigned long long)actual.offset, (unsigned long long)actual.sizelimit, (unsigned long long)capacity, actual.flags, end_read, temporary ? "true" : "false");
}

static void stock_init_loop_release(struct stock_init_loop *loop, long control, int temporary)
{
    /* LOOP_CLR_FD 完成最后一次 close 后释放 backing file；先卸载所有挂载。 */
    long request[6] = {loop->fd, 0x4c01};
    long cleared = call(29, request);
    REQUIRE(cleared == 0 && call(57, request) == 0);
    long opened[6] = {-100, (long)loop->device, 0};
    struct stock_init_loop_info info = {0};
    long status, status_waits = 0, remove_waits = 0;
    double deadline = stock_init_now() + 2;
    struct timespec delay = {0, 10000000};
    long sleep[6] = {(long)&delay};
    /* Host syscall 任务的 fput 经 delayed_fput_work 完成；查询 fd 每轮都关闭。 */
    for (;;) {
        long fd = call(56, opened);
        REQUIRE(fd >= 0);
        request[0] = fd;
        request[1] = 0x4c05;
        request[2] = (long)&info;
        status = call(29, request);
        REQUIRE(call(57, request) == 0);
        if (status == -6)
            break;
        if (!status_waits)
            fprintf(stderr, "stock_init_loop_deferred partition=%s status=%ld flags=%u\n", loop->name, status, info.flags);
        REQUIRE(status == 0 && (info.flags & 4) && stock_init_now() < deadline);
        REQUIRE(call(101, sleep) == 0);
        ++status_waits;
    }
    request[0] = control;
    request[1] = 0x4c81; /* LOOP_CTL_REMOVE */
    request[2] = loop->id;
    long removed;
    while ((removed = call(29, request)) != 0) {
        REQUIRE(removed == -16 && stock_init_now() < deadline);
        REQUIRE(call(101, sleep) == 0);
        ++remove_waits;
    }
    opened[2] = 0;
    REQUIRE(call(35, opened) == 0);
    loop->fd = -1;
    printf("{\"stock_init_sdk_loop_released\":true,\"partition\":\"%s\",\"loop_id\":%ld,\"clear_result\":%ld,\"status_result\":%ld,\"remove_result\":%ld,\"status_waits\":%ld,\"remove_waits\":%ld,\"temporary\":%s}\n",
           loop->name, loop->id, cleared, status, removed, status_waits, remove_waits, temporary ? "true" : "false");
}

static void stock_init_sdk_mount(const char *root)
{
    directory("/dev/block");
    long node[6] = {-100, (long)"/dev/loop-control", 0020000 | 0600, (10 << 8) | 237};
    REQUIRE(call(33, node) == 0);
    long opened[6] = {-100, (long)"/dev/loop-control", 2};
    long control = call(56, opened);
    REQUIRE(control >= 0);
    opened[1] = (long)"/dev/ncesystem";
    opened[2] = 0;
    long backing = call(56, opened);
    REQUIRE(backing >= 0);
    uint64_t capacity = 0;
    long size[6] = {backing, 0x80081272, (long)&capacity};
    REQUIRE(call(29, size) == 0);
    char path[4096], line[256];
    REQUIRE(snprintf(path, sizeof(path), "%s/original-partitions.tsv", root) < (int)sizeof(path));
    FILE *input = fopen(path, "r");
    REQUIRE(input);
    for (unsigned i = 0; i < sizeof(stock_init_loops) / sizeof(*stock_init_loops); ++i) {
        struct stock_init_loop *loop = &stock_init_loops[i];
        char name[32], filesystem[16], extra;
        unsigned long long offset, bytes;
        REQUIRE(fgets(line, sizeof(line), input) && strchr(line, '\n') && sscanf(line, "%31s\t%llu\t%llu\t%15s %c", name, &offset, &bytes, filesystem, &extra) == 4);
        REQUIRE(!strcmp(name, loop->name) && !strcmp(filesystem, loop->filesystem) && offset % 512 == 0 && bytes && bytes % 512 == 0 && offset <= capacity && bytes <= capacity - offset);
        loop->offset = offset;
        loop->size = bytes;
    }
    REQUIRE(!fgets(line, sizeof(line), input) && !ferror(input) && fclose(input) == 0);

    struct stock_init_loop probe = stock_init_loops[3];
    stock_init_loop_configure(&probe, control, backing, 1);
    directory("/stock-init-loop-check");
    long mount[6] = {(long)probe.device, (long)"/stock-init-loop-check", (long)"ext4", 1};
    REQUIRE(call(40, mount) == 0);
    opened[1] = (long)"/stock-init-loop-check/bin/hw/android.hardware.security.keymint-service";
    opened[2] = 1;
    long file_open = call(56, opened);
    REQUIRE(file_open == -30);
    opened[1] = (long)probe.device;
    long block_open = call(56, opened), block_write = block_open;
    if (block_open >= 0) {
        unsigned char byte = 0;
        long write[6] = {block_open, (long)&byte, 1};
        block_write = call(64, write);
        REQUIRE(block_write == -1 && call(57, write) == 0);
    } else
        REQUIRE(block_open == -13);
    long unmount[6] = {(long)"/stock-init-loop-check", 0};
    long unmounted = call(39, unmount);
    REQUIRE(unmounted == 0);
    printf("{\"stock_init_sdk_loop_lifecycle\":true,\"file_open_result\":%ld,\"block_open_result\":%ld,\"block_write_result\":%ld,\"unmount_result\":%ld}\n", file_open, block_open, block_write, unmounted);
    stock_init_loop_release(&probe, control, 1);
    for (unsigned i = 0; i < sizeof(stock_init_loops) / sizeof(*stock_init_loops); ++i) {
        struct stock_init_loop *loop = &stock_init_loops[i];
        stock_init_loop_configure(loop, control, backing, 0);
        directory(loop->mount);
        mount[0] = (long)loop->device;
        mount[1] = (long)loop->mount;
        mount[2] = (long)loop->filesystem;
        long result = call(40, mount);
        if (result) {
            fprintf(stderr, "stock_init_mount path=%s filesystem=%s result=%ld\n", loop->mount, loop->filesystem, result);
            stock_init_dump_kernel_log();
        }
        REQUIRE(result == 0);
        if (i)
            printf("{\"stock_init_original_partition_mounted\":true,\"path\":\"%s\",\"filesystem\":\"%s\",\"readonly\":true}\n", loop->mount, loop->filesystem);
    }
    opened[0] = backing;
    REQUIRE(call(57, opened) == 0);
    opened[0] = control;
    REQUIRE(call(57, opened) == 0);
}

static void stock_init_sdk_cleanup(int lifecycle)
{
    /* setup 创建了分区、自身 bind 及原根文件 bind，按相反顺序卸载。 */
    const char *mounts[] = {"/init.environ.rc", "/system", "/vendor", "/product", "/system_ext", "/system", "/system_dlkm", "/vendor", "/system_ext", "/product", "/system-image"};
    for (unsigned i = 0; i < sizeof(mounts) / sizeof(*mounts); ++i) {
        long unmount[6] = {(long)mounts[i], 0};
        long result = call(39, unmount);
        if (result)
            fprintf(stderr, "stock_init_unmount path=%s result=%ld\n", mounts[i], result);
        if (!lifecycle && i == 0 && (result == -1 || result == -13)) {
            /* 原策略限制 kernel 域卸载；已持有的 fd 关闭后由标准 halt 结束内核。 */
            unsigned count = sizeof(stock_init_loops) / sizeof(*stock_init_loops);
            for (unsigned j = 0; j < count; ++j) {
                long close[6] = {stock_init_loops[j].fd};
                REQUIRE(call(57, close) == 0);
                stock_init_loops[j].fd = -1;
            }
            printf("{\"stock_init_sdk_cleanup_deferred\":true,\"path\":\"%s\",\"unmount_result\":%ld,\"closed_loop_fds\":%u}\n", mounts[i], result, count);
            return;
        }
        REQUIRE(result == 0);
    }
    long opened[6] = {-100, (long)"/dev/loop-control", 2};
    long control = call(56, opened);
    REQUIRE(control >= 0);
    for (unsigned i = sizeof(stock_init_loops) / sizeof(*stock_init_loops); i-- > 0;)
        stock_init_loop_release(&stock_init_loops[i], control, 0);
    opened[0] = control;
    REQUIRE(call(57, opened) == 0);
}
