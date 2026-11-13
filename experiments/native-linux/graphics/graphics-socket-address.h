/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef __KERNEL__
#include <linux/stddef.h>
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Linux sockaddr_un 使用 16-bit family；Darwin 的布局还包含长度字节。 */
struct nce_graphics_socket_address {
    uint16_t family;
    char name[108];
};

#define NCE_GRAPHICS_SOCKET_NAME "\0nce-render"
#define NCE_GRAPHICS_SOCKET_ADDRESS_INIT {1, NCE_GRAPHICS_SOCKET_NAME}
/* abstract 地址保留开头的 NUL，长度排除字面量末尾的 NUL 和结构体填充。 */
enum {
    NCE_GRAPHICS_SOCKET_ADDRESS_BYTES = offsetof(struct nce_graphics_socket_address, name) + sizeof(NCE_GRAPHICS_SOCKET_NAME) - 1
};
