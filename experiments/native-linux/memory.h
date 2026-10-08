/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"

/* Code ranges are prepared separately from the ordinary data reservation.
 * Slices are page aligned and remain valid until nce_memory_release(). */
int nce_memory_prepare(void);
NceCode nce_memory_take(size_t size);
/* The surrounding Linux address range follows layout.h. */
NceCode nce_memory_guest_code(void);
/* Replace an ordinary data mapping and apply POSIX protection flags. Logs
 * the Mach error or errno and returns -1 on failure. */
int nce_memory_remap(void *target, void *source, size_t size, int protection);
int nce_memory_release(void);
