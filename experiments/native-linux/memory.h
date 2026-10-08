/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"

/* One prepared region owns kernel code, Linux physical RAM and NCE code.
 * Slices are page aligned and remain valid until nce_memory_release(). */
int nce_memory_prepare(void);
NceCode nce_memory_take(size_t size);
void nce_memory_release(void);
