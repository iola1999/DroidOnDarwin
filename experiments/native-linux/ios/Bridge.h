/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_IOS_BRIDGE_H
#define NCE_IOS_BRIDGE_H
#include <stdbool.h>
int nce_lkl_run(const char *image_path);
int nce_open_log(const char *path);
void nce_log(const char *message);
bool nce_debugger_attached(void);
void husk_ios_jit_install_trap_handler(void);
#endif
