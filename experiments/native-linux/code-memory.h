/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Prepare a caller-owned, 16 KiB aligned range as RX and create an RW alias.
 * iOS callers must prepare all ranges before detaching the debugger. No thread
 * may execute or write the range during preparation. A zero result reports
 * failure; the caller still owns the original range. On success the caller
 * must release the RW alias as well as its original mapping after execution.
 * Data adjacent to the supplied range is not remapped or protected. */
NceCode nce_code_prepare_range(void *address, size_t size);
/* Release an owned range after all users have stopped. iOS prepared mappings
 * may remain until this single-run process exits; that case is logged. */
int nce_code_release_range(void *address, size_t size, int prepared);
#ifdef __cplusplus
}
#endif
