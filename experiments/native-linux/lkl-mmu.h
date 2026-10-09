/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LKL_NCE_MMU_H
#define _LKL_NCE_MMU_H
#ifdef CONFIG_MMU
#include <asm/nce-layout.h>
typedef struct {
    unsigned long sigreturn;
    /* Pages faulted back into the host window after a process switch. */
    unsigned long *accessed;
} mm_context_t;
#else
#include <asm-generic/mmu.h>
#endif
#endif
