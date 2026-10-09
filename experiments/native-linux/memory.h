/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"

/* Code ranges are prepared separately from the ordinary data reservation.
 * Slices are page aligned and remain valid until nce_memory_release(). */
int nce_memory_prepare(void);
NceCode nce_memory_take(size_t size);
/* Fixed executable window for Linux file-backed dynamic code. */
NceCode nce_memory_dynamic_code(void);
/* The surrounding Linux address range follows layout.h. */
uintptr_t nce_memory_user_base(void);
/* Replace an ordinary data mapping and apply POSIX protection flags. Logs
 * the Mach error or errno and returns -1 on failure. */
int nce_memory_remap(void *target, void *source, size_t size, int protection);
/* True only after prepared code survives permission removal and restoration. */
int nce_memory_code_protection(void);
int nce_memory_release(void);
