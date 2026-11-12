/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "stock-init-volume.h"

static struct stock_init_volume stock_init_metadata = {"metadata", -1, 0};

static void stock_init_metadata_read(const char *path, char *text, size_t size)
{
    long args[6] = {-100, (long)path, 0};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    args[0] = fd;
    args[1] = (long)text;
    args[2] = size - 1;
    long count = call(63, args);
    REQUIRE(count > 0 && count < (long)size && call(57, args) == 0);
    text[count] = 0;
}

static uint64_t stock_init_metadata_number(const char *path)
{
    char text[64], extra;
    unsigned long long value;
    stock_init_metadata_read(path, text, sizeof(text));
    REQUIRE(sscanf(text, "%llu %c", &value, &extra) == 1);
    return value;
}

static uint64_t stock_init_metadata_node(const char *sysfs, const char *path, unsigned mode)
{
    char text[64], extra;
    unsigned major, minor;
    stock_init_metadata_read(sysfs, text, sizeof(text));
    REQUIRE(sscanf(text, "%u:%u %c", &major, &minor, &extra) == 2 && major < 4096 && minor < (1U << 20));
    /* mknodat 使用 Linux new_encode_dev，扩展分区 minor 可以超过 255。 */
    uint32_t encoded = (major << 8) | (minor & 255) | ((minor & ~255U) << 12);
    long node[6] = {-100, (long)path, 0060000 | mode, encoded};
    REQUIRE(call(33, node) == 0);
    struct stock_init_volume_status status = {0};
    uint64_t device = ((uint64_t)major << 32) | minor;
    REQUIRE(stock_init_volume_stat(-100, path, &status) == 0 && status.rdev == device && (status.mode & 0170000) == 0060000);
    return device;
}

static void stock_init_metadata_mount(void)
{
    const char *image = getenv("NCE_STOCK_INIT_METADATA");
    if (!image || !image[0])
        return;
    REQUIRE(getenv("NCE_STOCK_INIT_LP") && !strcmp(getenv("NCE_STOCK_INIT_LP"), "1") &&
            (!getenv("NCE_STOCK_INIT_VENDOR") || !getenv("NCE_STOCK_INIT_VENDOR")[0]));
    char path[4096], name[64], extra;
    unsigned expected_partition;
    unsigned long long expected_start, expected_sectors, expected_whole;
    REQUIRE(snprintf(path, sizeof(path), "%s/metadata-partition.tsv", getenv("NCE_STOCK_INIT_DIRECTORY")) < (int)sizeof(path));
    FILE *input = fopen(path, "r");
    REQUIRE(input && fscanf(input, "%63s %u %llu %llu %llu %c", name, &expected_partition, &expected_start, &expected_sectors, &expected_whole, &extra) == 5 && fclose(input) == 0);
    uint64_t start = stock_init_metadata_number("/sys/block/ncedisk2/ncedisk2p1/start");
    uint64_t sectors = stock_init_metadata_number("/sys/block/ncedisk2/ncedisk2p1/size");
    uint64_t whole = stock_init_metadata_number("/sys/block/ncedisk2/size");
    uint64_t partition = stock_init_metadata_number("/sys/block/ncedisk2/ncedisk2p1/partition");
    char event[1024];
    stock_init_metadata_read("/sys/block/ncedisk2/ncedisk2p1/uevent", event, sizeof(event));
    char expected_name[96];
    REQUIRE(snprintf(expected_name, sizeof(expected_name), "\nPARTNAME=%s\n", name) < (int)sizeof(expected_name));
    REQUIRE(strstr(event, expected_name) && !strcmp(name, "metadata"));
    REQUIRE(start == expected_start && sectors == expected_sectors && whole == expected_whole &&
            partition == expected_partition && start < whole && sectors <= whole - start && sectors <= UINT64_MAX / 512);
    uint64_t whole_device = stock_init_metadata_node("/sys/block/ncedisk2/dev", "/dev/ncemetadata", 0400);
    uint64_t device = stock_init_metadata_node("/sys/block/ncedisk2/ncedisk2p1/dev", "/dev/block/vdd", 0600);
    REQUIRE((uint32_t)whole_device == 2 && (device >> 32) == 259 && device != whole_device);
    stock_init_metadata.device = device;
    long opened[6] = {-100, (long)"/dev/block/vdd", 0};
    long fd = call(56, opened);
    opened[1] = (long)"/dev/ncemetadata";
    long whole_fd = call(56, opened);
    REQUIRE(fd >= 0 && whole_fd >= 0);
    uint64_t bytes = 0;
    long size[6] = {fd, 0x80081272, (long)&bytes}; /* BLKGETSIZE64 */
    REQUIRE(call(29, size) == 0 && bytes == sectors * 512);
    int readonly = -1;
    long ro[6] = {fd, 0x125e, (long)&readonly}; /* BLKROGET */
    REQUIRE(call(29, ro) == 0 && readonly == 0);
    unsigned char superblock[1024], whole_superblock[1024], byte;
    long read[6] = {fd, (long)superblock, sizeof(superblock), 1024};
    REQUIRE(call(67, read) == sizeof(superblock));
    read[0] = whole_fd;
    read[1] = (long)whole_superblock;
    read[3] = start * 512 + 1024;
    REQUIRE(call(67, read) == sizeof(whole_superblock) && !memcmp(superblock, whole_superblock, sizeof(superblock)) &&
            superblock[56] == 0x53 && superblock[57] == 0xef);
    read[0] = fd;
    read[1] = (long)&byte;
    read[2] = 1;
    read[3] = bytes;
    long end = call(67, read);
    read[3] = bytes + 512;
    long beyond = call(67, read);
    long close[6] = {fd};
    REQUIRE(call(57, close) == 0);
    close[0] = whole_fd;
    REQUIRE(call(57, close) == 0);
    printf("{\"stock_init_metadata_partition\":true,\"name\":\"%s\",\"partition\":%llu,\"start_sector\":%llu,\"sectors\":%llu,\"whole_sectors\":%llu,\"whole_device\":%llu,\"device\":%llu,\"bytes\":%llu,\"readonly\":%d,\"superblock_equal\":true,\"ext4_magic\":61267,\"end_read_result\":%ld,\"beyond_read_result\":%ld}\n", name, (unsigned long long)partition, (unsigned long long)start, (unsigned long long)sectors, (unsigned long long)whole, (unsigned long long)whole_device, (unsigned long long)device, (unsigned long long)bytes, readonly, end, beyond);
    REQUIRE(end == 0 && beyond == 0);
    long mount[6] = {(long)"/dev/block/vdd", (long)"/metadata", (long)"ext4", 1030, 0};
    long mounted = call(40, mount);
    printf("{\"stock_init_metadata_mount\":true,\"device\":%llu,\"flags\":1030,\"mount_result\":%ld}\n", (unsigned long long)device, mounted);
    REQUIRE(mounted == 0);
}

static int stock_init_metadata_finish(int lifecycle)
{
    if (stock_init_metadata.fd < 0)
        return 0;
    /* 只观察固定目录的属性，不遍历目录或读取 vold/APEX 文件内容。 */
    const char *paths[] = {"/metadata", "/metadata/vold", "/metadata/apex", "/metadata/apex/sessions"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(*paths); ++i) {
        struct stock_init_volume_status status = {0};
        long info = stock_init_volume_stat(-100, paths[i], &status);
        unsigned char label[256] = {0};
        long attribute[6] = {(long)paths[i], (long)"security.selinux", (long)label, sizeof(label)};
        long result = call(9, attribute);
        flockfile(stdout);
        printf("{\"stock_init_metadata_directory\":true,\"path\":\"%s\",\"stat_result\":%ld,\"device\":%llu,\"mode\":%u,\"inode_type\":%u,\"uid\":%u,\"gid\":%u,\"label_result\":%ld,\"label_hex\":\"", paths[i], info, (unsigned long long)status.device, status.mode & 07777, status.mode & 0170000, status.uid, status.gid, result);
        for (long j = 0; j < result && j < (long)sizeof(label); ++j)
            printf("%02x", label[j]);
        puts("\"}");
        funlockfile(stdout);
    }
    long args[6] = {stock_init_metadata.fd};
    long result = call(82, args);
    printf("{\"stock_init_metadata_directory_sync\":true,\"fd\":%ld,\"result\":%ld}\n", stock_init_metadata.fd, result);
    int failed = stock_init_volume_cleanup(&stock_init_metadata, lifecycle);
    return result != 0 || failed;
}
