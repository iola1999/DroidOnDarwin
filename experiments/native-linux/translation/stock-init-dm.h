/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "stock-init-dm-abi.h"

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
