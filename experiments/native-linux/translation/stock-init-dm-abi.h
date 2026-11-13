/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Darwin 宿主使用固定 Linux dm-ioctl.h 的结构布局。 */
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
enum {
    STOCK_INIT_DM_REMOVE_ALL = 1,
    STOCK_INIT_DM_LIST_DEVICES = 2,
    STOCK_INIT_DM_BUFFER_FULL_FLAG = 1U << 8,
};
_Static_assert(sizeof(struct stock_init_dm_info) == 312, "Linux dm_ioctl ABI");
_Static_assert(offsetof(struct stock_init_dm_info, data) == 305, "Linux dm_ioctl minimum copy size");
_Static_assert(sizeof(struct stock_init_dm_target) == 40, "Linux dm_target_spec ABI");
