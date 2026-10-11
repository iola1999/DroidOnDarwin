/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
/* Linux LKL's 16 KiB, three-level page table. The kernel checks these values. */
#define DIRECT_PTE_PAGE_SHIFT 14
#define DIRECT_PTE_PMD_SHIFT 25
#define DIRECT_PTE_PGD_SHIFT 36
#define DIRECT_PTE_INDEX_BITS 11
#define DIRECT_PTE_PRESENT 0x001
#define DIRECT_PTE_PROTNONE 0x010
#define DIRECT_PTE_WRITE 0x020
#define DIRECT_PTE_USER 0x040
#define DIRECT_PTE_YOUNG 0x080
#define DIRECT_PTE_DIRTY 0x100
#define DIRECT_PTE_READ_REQUIRED 0x0c1
#define DIRECT_PTE_WRITE_REQUIRED 0x1e1
