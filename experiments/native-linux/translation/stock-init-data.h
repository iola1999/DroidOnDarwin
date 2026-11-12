/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

static long stock_init_data_fd = -1;
static uint64_t stock_init_data_device;

static long stock_init_data_stat(long fd, const char *path, uint64_t *device, uint64_t *rdev)
{
    /* Linux statx 的 256-byte ABI；设备号字段从 byte 128 开始。 */
    unsigned char status[256] = {0};
    long args[6] = {fd, (long)path, fd == -100 ? 0 : 0x1000, 0x7ff, (long)status};
    long result = call(291, args);
    if (!result) {
        uint32_t numbers[4];
        memcpy(numbers, status + 128, sizeof(numbers));
        *rdev = ((uint64_t)numbers[0] << 32) | numbers[1];
        *device = ((uint64_t)numbers[2] << 32) | numbers[3];
    }
    return result;
}

static void stock_init_data_mount(void)
{
    const char *image = getenv("NCE_TRANSLATION_DATA");
    if (!image || !image[0])
        return;
    block_device("/sys/block/ncedisk0/dev", "/dev/block/vdc", 0600);
    uint64_t dev, rdev;
    REQUIRE(stock_init_data_stat(-100, "/dev/block/vdc", &dev, &rdev) == 0);
    stock_init_data_device = rdev;
    long mount[6] = {(long)"/dev/block/vdc", (long)"/data", (long)"ext4", 1030, (long)"errors=panic"};
    long result = call(40, mount);
    printf("{\"stock_init_data_mount\":true,\"device\":%llu,\"flags\":1030,\"mount_result\":%ld}\n", (unsigned long long)rdev, result);
    REQUIRE(result == 0);
}

static void stock_init_data_retain(void)
{
    const char *image = getenv("NCE_TRANSLATION_DATA");
    if (!image || !image[0])
        return;
    long open[6] = {-100, (long)"/data", 0x84000}; /* arm64 Linux O_DIRECTORY | O_CLOEXEC */
    stock_init_data_fd = call(56, open);
    printf("{\"stock_init_data_open\":true,\"fd\":%ld,\"flags\":%ld}\n", stock_init_data_fd, open[2]);
    REQUIRE(stock_init_data_fd >= 3);
    uint64_t device, rdev;
    REQUIRE(stock_init_data_stat(stock_init_data_fd, "", &device, &rdev) == 0 && device == stock_init_data_device);
    /* arm64 Linux statfs 使用 120-byte ABI。 */
    long filesystem[15] = {0};
    long statfs[6] = {stock_init_data_fd, (long)filesystem};
    REQUIRE(call(44, statfs) == 0 && filesystem[0] == 0xef53);
    long flags[6] = {stock_init_data_fd, 1}; /* F_GETFD */
    REQUIRE(call(25, flags) == 1);
    char text[64];
    REQUIRE(snprintf(text, sizeof(text), "%ld %llu\n", stock_init_data_fd, (unsigned long long)device) > 0);
    put_text("/stock-init-data", text);
    printf("{\"stock_init_data_retained\":true,\"fd\":%ld,\"device\":%llu,\"filesystem_type\":%ld,\"cloexec\":true}\n", stock_init_data_fd, (unsigned long long)device, filesystem[0]);
}

static int stock_init_data_cleanup(int lifecycle)
{
    if (stock_init_data_fd < 0)
        return 0;
    uint64_t device = 0, rdev = 0, current = 0;
    long held_stat = stock_init_data_stat(stock_init_data_fd, "", &device, &rdev);
    long path_stat = stock_init_data_stat(-100, "/data", &current, &rdev);
    /* syncfs 使用 launch 前保存的普通目录 fd，Linux workers 此时仍运行。 */
    long args[6] = {stock_init_data_fd};
    long sync = call(267, args);
    long closed = call(57, args);
    stock_init_data_fd = -1;
    long unmount[6] = {(long)"/data", 0};
    long removed = call(39, unmount);
    printf("{\"stock_init_data_cleanup\":true,\"held_stat_result\":%ld,\"path_stat_result\":%ld,\"held_device\":%llu,\"path_device\":%llu,\"sync_result\":%ld,\"close_result\":%ld,\"unmount_result\":%ld}\n", held_stat, path_stat, (unsigned long long)device, (unsigned long long)current, sync, closed, removed);
    return sync != 0 || closed != 0 || held_stat != 0 || path_stat != 0 ||
           device != stock_init_data_device || current != device ||
           (lifecycle ? removed != 0 : removed != 0 && removed != -1 && removed != -13);
}
