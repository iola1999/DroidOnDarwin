/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_IOS_BRIDGE_H
#define NCE_IOS_BRIDGE_H
#include <stdbool.h>
#ifdef __OBJC__
#import "../app/DeviceStore.h"
#import "../macos/Storage.h"
#endif
#include "../input.h"
#include "../session.h"
int nce_lkl_run(const char *image_path);
int nce_open_log(const char *path);
void nce_log(const char *message);
bool nce_debugger_attached(void);
/* Waits on the worker thread. Returns 0 after detach, -1 on query failure,
 * or -2 on timeout; these failures leave the UI available for log export. */
int nce_wait_debugger_detach(unsigned timeout_ms);
void dod_ios_jit_install_trap_handler(void);
void nce_graphics_set_window(void *layer);
#endif
