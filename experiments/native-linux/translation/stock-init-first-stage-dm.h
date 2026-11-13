/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "stock-init-dm.h"

/* 父任务持有唯一 misc control fd，原 ramdisk 的删除不影响这个引用。 */
static long stock_init_first_stage_dm_control = -1;

static void stock_init_first_stage_dm_control_result(const char *phase, const char *operation, long fd, long result)
{
    printf("{\"stock_init_first_stage_dm_control\":true,\"phase\":\"%s\",\"operation\":\"%s\",\"fd\":%ld,\"result\":%ld}\n", phase, operation, fd, result);
}

static long stock_init_first_stage_dm_close(const char *phase, const char *operation, long fd)
{
    long args[6] = {fd};
    long result = call(57, args);
    stock_init_first_stage_dm_control_result(phase, operation, fd, result);
    return result;
}

static void stock_init_first_stage_dm_ioctl_result(const char *phase, unsigned command, long result, const union stock_init_dm_message *message)
{
    uint64_t first_dev = 0;
    int observed = command == STOCK_INIT_DM_LIST_DEVICES && message->info.data_start == sizeof(message->info) &&
                   message->info.data_start <= sizeof(*message) - sizeof(first_dev);
    if (observed)
        memcpy(&first_dev, message->data + message->info.data_start, sizeof(first_dev));
    int within_data_size = observed && result == 0 && message->info.data_size <= sizeof(*message) &&
                           message->info.data_size >= message->info.data_start + sizeof(first_dev);
    printf("{\"stock_init_first_stage_dm_ioctl\":true,\"phase\":\"%s\",\"command\":%u,\"result\":%ld,\"version\":[%u,%u,%u],\"flags\":%u,\"data_size\":%u,\"data_start\":%u,\"dev\":%llu,\"first_dev_observed\":%s,\"first_dev_within_data_size\":%s,\"first_dev\":%llu}\n",
           phase, command, result, message->info.version[0], message->info.version[1], message->info.version[2],
           message->info.flags, message->info.data_size, message->info.data_start, (unsigned long long)message->info.dev,
           observed ? "true" : "false", within_data_size ? "true" : "false", (unsigned long long)first_dev);
}

/* 部署原 ramdisk 前调用；失败也关闭已产生的 fd 并尝试删除临时节点。 */
static long stock_init_first_stage_dm_prepare(void)
{
    long result = 0, control = -1;
    if (stock_init_first_stage_dm_control >= 0) {
        result = -16;
        goto out;
    }
    long node[6] = {-100, (long)"/stock-init-dm-control", 0020000 | 0600, (10 << 8) | 236};
    result = call(33, node);
    stock_init_first_stage_dm_control_result("prepare", "mknod", -1, result);
    if (result)
        goto out;
    long opened[6] = {-100, (long)"/stock-init-dm-control", 2};
    long fd = call(56, opened);
    stock_init_first_stage_dm_control_result("prepare", "open", fd >= 0 ? fd : -1, fd);
    if (fd >= 0) {
        long duplicate[6] = {fd, 1030, 3}; /* F_DUPFD_CLOEXEC */
        long retained = call(25, duplicate);
        stock_init_first_stage_dm_control_result("prepare", "duplicate", fd, retained);
        if (retained >= 0)
            control = retained;
        if (retained < 3)
            result = retained < 0 ? retained : -22;
        long closed = stock_init_first_stage_dm_close("prepare", "close_initial", fd);
        if (!result)
            result = closed;
    } else
        result = fd;
    long unlink[6] = {-100, (long)"/stock-init-dm-control", 0};
    long unlinked = call(35, unlink);
    stock_init_first_stage_dm_control_result("prepare", "unlink", -1, unlinked);
    if (!result)
        result = unlinked;
    if (!result) {
        union stock_init_dm_message message;
        stock_init_dm_prepare(&message, NULL, 0);
        result = stock_init_dm_ioctl(control, 0, &message); /* DM_VERSION */
        stock_init_first_stage_dm_ioctl_result("prepare", 0, result, &message);
        if (!result && (message.info.version[0] != 4 || message.info.version[1] < 48))
            result = -22;
    }
    if (result && control >= 0)
        stock_init_first_stage_dm_close("prepare", "close_control", control);
    if (!result)
        stock_init_first_stage_dm_control = control;
out:
    printf("{\"stock_init_first_stage_dm_prepare\":true,\"result\":%ld,\"control_fd\":%ld}\n", result, stock_init_first_stage_dm_control);
    return result;
}

/* 调用方完成实际 wait/reaped 与 sync 后调用。失败返回，后续 halt 仍由调用方完成。 */
static long stock_init_first_stage_dm_cleanup(void)
{
    long control = stock_init_first_stage_dm_control;
    long result = -9;
    int empty = 0;
    if (control >= 0) {
        union stock_init_dm_message message;
        stock_init_dm_prepare(&message, NULL, 0);
        result = stock_init_dm_ioctl(control, STOCK_INIT_DM_REMOVE_ALL, &message);
        stock_init_first_stage_dm_ioctl_result("cleanup", STOCK_INIT_DM_REMOVE_ALL, result, &message);
        stock_init_dm_prepare(&message, NULL, 0);
        long listed = stock_init_dm_ioctl(control, STOCK_INIT_DM_LIST_DEVICES, &message);
        stock_init_first_stage_dm_ioctl_result("cleanup", STOCK_INIT_DM_LIST_DEVICES, listed, &message);
        long checked = listed;
        if (!checked) {
            if (message.info.version[0] != 4 || message.info.version[1] < 48)
                checked = -22;
            else if (message.info.flags & STOCK_INIT_DM_BUFFER_FULL_FLAG)
                checked = -75;
            else if (message.info.data_start != sizeof(message.info) || message.info.data_size < message.info.data_start ||
                     message.info.data_size > sizeof(message) ||
                     (message.info.data_size > message.info.data_start && message.info.data_size - message.info.data_start < sizeof(uint64_t)))
                checked = -22;
            else {
                /* 空结果的 8-byte dev 不在回写范围内；这里只用返回 header 判定。 */
                empty = message.info.data_size == message.info.data_start;
                if (!empty)
                    checked = -16;
            }
        }
        if (!result)
            result = checked;
        /* Linux close 先清除 fd 表项，错误返回也不能重新关闭同一 fd。 */
        stock_init_first_stage_dm_control = -1;
        long closed = stock_init_first_stage_dm_close("cleanup", "close_control", control);
        if (!result)
            result = closed;
    }
    printf("{\"stock_init_first_stage_dm_cleanup\":true,\"result\":%ld,\"control_fd\":%ld,\"empty\":%s}\n", result, control, empty ? "true" : "false");
    return result;
}
