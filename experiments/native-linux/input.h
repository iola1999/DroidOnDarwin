/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

enum { NCE_INPUT_DOWN, NCE_INPUT_MOVE, NCE_INPUT_UP, NCE_INPUT_KEY };
typedef struct { int32_t action, x, y; } NceInputCommand;

#ifdef __cplusplus
extern "C" {
#endif
void nce_host_input_start(long (*linux_syscall)(long, long *));
void nce_host_input_stop(void);
/* Returns zero after delivery to the Linux socket, or a negative Linux errno.
 * Call from the UI's serial input queue; Linux may wait for its scheduler. */
int nce_host_input_send(int action, int x, int y);
#ifdef __cplusplus
}
#endif
