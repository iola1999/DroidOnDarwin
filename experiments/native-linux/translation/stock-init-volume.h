/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

struct stock_init_volume {
    const char *name;
    long fd;
    uint64_t device;
};

struct stock_init_volume_status {
    uint64_t device, rdev;
    uint32_t uid, gid;
    uint16_t mode;
};

static long stock_init_volume_stat(long fd, const char *path, struct stock_init_volume_status *info)
{
    /* Linux statx 的 256-byte ABI；设备号字段从 byte 128 开始。 */
    unsigned char status[256] = {0};
    long args[6] = {fd, (long)path, fd == -100 ? 0 : 0x1000, 0x7ff, (long)status};
    long result = call(291, args);
    if (!result) {
        uint32_t numbers[4];
        memcpy(numbers, status + 128, sizeof(numbers));
        info->rdev = ((uint64_t)numbers[0] << 32) | numbers[1];
        info->device = ((uint64_t)numbers[2] << 32) | numbers[3];
        memcpy(&info->uid, status + 20, sizeof(info->uid));
        memcpy(&info->gid, status + 24, sizeof(info->gid));
        memcpy(&info->mode, status + 28, sizeof(info->mode));
    }
    return result;
}

static void stock_init_volume_retain(struct stock_init_volume *volume)
{
    if (!volume->device)
        return;
    char path[64];
    REQUIRE(snprintf(path, sizeof(path), "/%s", volume->name) < (int)sizeof(path));
    long open[6] = {-100, (long)path, 0x84000}; /* arm64 Linux O_DIRECTORY | O_CLOEXEC */
    volume->fd = call(56, open);
    printf("{\"stock_init_%s_open\":true,\"fd\":%ld,\"flags\":%ld}\n", volume->name, volume->fd, open[2]);
    REQUIRE(volume->fd >= 3);
    struct stock_init_volume_status status = {0};
    REQUIRE(stock_init_volume_stat(volume->fd, "", &status) == 0 && status.device == volume->device);
    /* arm64 Linux statfs 使用 120-byte ABI。 */
    long filesystem[15] = {0};
    long statfs[6] = {volume->fd, (long)filesystem};
    REQUIRE(call(44, statfs) == 0 && filesystem[0] == 0xef53);
    long flags[6] = {volume->fd, 1}; /* F_GETFD */
    REQUIRE(call(25, flags) == 1);
    char text[64];
    REQUIRE(snprintf(text, sizeof(text), "%ld %llu\n", volume->fd, (unsigned long long)status.device) > 0);
    REQUIRE(snprintf(path, sizeof(path), "/stock-init-%s", volume->name) < (int)sizeof(path));
    put_text(path, text);
    printf("{\"stock_init_%s_retained\":true,\"fd\":%ld,\"device\":%llu,\"filesystem_type\":%ld,\"cloexec\":true}\n", volume->name, volume->fd, (unsigned long long)status.device, filesystem[0]);
}

static int stock_init_volume_cleanup(struct stock_init_volume *volume, int lifecycle)
{
    if (volume->fd < 0)
        return 0;
    struct stock_init_volume_status held = {0}, path = {0};
    char directory[64];
    REQUIRE(snprintf(directory, sizeof(directory), "/%s", volume->name) < (int)sizeof(directory));
    long held_stat = stock_init_volume_stat(volume->fd, "", &held);
    long path_stat = stock_init_volume_stat(-100, directory, &path);
    /* namespace wait 完成后同步保存的目录 fd；Linux workers 此时仍运行。 */
    long args[6] = {volume->fd};
    long sync = call(267, args);
    long closed = call(57, args);
    volume->fd = -1;
    long unmount[6] = {(long)directory, 0};
    long removed = call(39, unmount);
    printf("{\"stock_init_%s_cleanup\":true,\"held_stat_result\":%ld,\"path_stat_result\":%ld,\"held_device\":%llu,\"path_device\":%llu,\"sync_result\":%ld,\"close_result\":%ld,\"unmount_result\":%ld}\n", volume->name, held_stat, path_stat, (unsigned long long)held.device, (unsigned long long)path.device, sync, closed, removed);
    /* 原 metadata_file 策略拒绝宿主 getattr；身份由 launch 前与同盘 recovery 核对。 */
    int denied = !lifecycle && !strcmp(volume->name, "metadata") && held_stat == -13 && path_stat == -13 && held.device == 0 && path.device == 0;
    return sync != 0 || closed != 0 || (!denied && (held_stat != 0 || path_stat != 0 || held.device != volume->device || path.device != held.device)) ||
           (lifecycle ? removed != 0 : removed != 0 && removed != -1 && removed != -13);
}
