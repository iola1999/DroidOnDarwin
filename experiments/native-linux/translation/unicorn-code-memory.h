/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Prepare the next Unicorn cache on this thread before engine initialization.
 * This reports preparation failure before upstream initialization can exit the
 * process. The next allocation must request the same size. Cancel if unused. */
int nce_uc_code_reserve(size_t size);
void nce_uc_code_cancel_reservation(void);
/* All engines must be idle. Serialize with all other execution-memory checks. */
int nce_uc_code_check_all(void);
#ifdef __cplusplus
}
#endif
