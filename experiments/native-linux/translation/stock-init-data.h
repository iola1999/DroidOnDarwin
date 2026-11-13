/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "stock-init-volume.h"

static struct stock_init_volume stock_init_data = {"data", -1, 0};

static void stock_init_data_mount(void)
{
    const char *image = getenv("NCE_TRANSLATION_DATA");
    if (!image || !image[0])
        return;
    block_device("/sys/block/ncedisk0/dev", "/dev/block/vdc", 0600);
    struct stock_init_volume_status status = {0};
    REQUIRE(stock_init_volume_stat(-100, "/dev/block/vdc", &status) == 0);
    stock_init_data.device = status.rdev;
    long mount[6] = {(long)"/dev/block/vdc", (long)"/data", (long)"ext4", 1030, (long)"errors=panic"};
    long result = call(40, mount);
    printf("{\"stock_init_data_mount\":true,\"device\":%llu,\"flags\":1030,\"mount_result\":%ld}\n", (unsigned long long)status.rdev, result);
    REQUIRE(result == 0);
}
