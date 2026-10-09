/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#define NCE_GPU_WIDTH 360
#define NCE_GPU_HEIGHT 640
#ifdef __cplusplus
extern "C" {
#endif
int nce_graphics_start(long (*linux_syscall)(long, long *));
void nce_graphics_stop(void);
/* Called before start; the caller retains this CAMetalLayer until stop. */
void nce_graphics_set_window(void *layer);
#ifdef __cplusplus
}
#endif
