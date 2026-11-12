/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../layout.h"
/* Shared with LKL; the kernel checks the three-level layout. */
#define DIRECT_PTE_PAGE_SHIFT NCE_GUEST_PAGE_SHIFT
#define DIRECT_PTE_INDEX_BITS (NCE_GUEST_PAGE_SHIFT - 3)
#define DIRECT_PTE_PMD_SHIFT (DIRECT_PTE_PAGE_SHIFT + DIRECT_PTE_INDEX_BITS)
#define DIRECT_PTE_PGD_SHIFT (DIRECT_PTE_PMD_SHIFT + DIRECT_PTE_INDEX_BITS)
#define DIRECT_PTE_PRESENT 0x001
#define DIRECT_PTE_PROTNONE 0x010
#define DIRECT_PTE_WRITE 0x020
#define DIRECT_PTE_USER 0x040
#define DIRECT_PTE_YOUNG 0x080
#define DIRECT_PTE_DIRTY 0x100
#define DIRECT_PTE_READ_REQUIRED 0x0c1
#define DIRECT_PTE_WRITE_REQUIRED 0x1e1
#define DIRECT_FLAT_USER_BITS 32
#define DIRECT_FLAT_BYTES (1UL << (DIRECT_FLAT_USER_BITS - DIRECT_PTE_PAGE_SHIFT + 4))
#ifndef __ASSEMBLER__
/* Zero requires Linux fault handling. Nonzero entries point to RAM pages. */
struct nce_direct_flat_page {
    unsigned long read, write;
};
#endif
