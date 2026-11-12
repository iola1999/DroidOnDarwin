/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_LAYOUT_H
#define NCE_LAYOUT_H
#ifndef NCE_GUEST_PAGE_SHIFT
#define NCE_GUEST_PAGE_SHIFT 14
#endif
#if NCE_GUEST_PAGE_SHIFT != 12 && NCE_GUEST_PAGE_SHIFT != 14
#error Unsupported guest page size
#endif
#define NCE_PAGE_SIZE (1UL << NCE_GUEST_PAGE_SHIFT)
#define NCE_HOST_PAGE_SIZE 16384UL
#define NCE_CODE_POOL_SIZE (16UL * 1024 * 1024)
#define NCE_USER_SIZE (4UL * 1024 * 1024 * 1024)
/* Linker and libdl size their CFI shadow for this Darwin address range. */
#define NCE_HOST_ADDRESS_LIMIT (1UL << (NCE_GUEST_PAGE_SHIFT == 12 ? 39 : 40))
#define NCE_RAM_SIZE 0x40000000UL
#define NCE_VMALLOC_SIZE 0x4000000UL
#define NCE_EXEC_SIZE (64UL * 1024 * 1024)
#define NCE_EXEC_DATA_OFFSET 0x20000000UL
#define NCE_EXEC_OFFSET (NCE_EXEC_DATA_OFFSET + NCE_EXEC_SIZE)
#define NCE_BRK_OFFSET (NCE_EXEC_OFFSET + NCE_EXEC_SIZE)
/* Keep initial brk growth clear of loader metadata and anonymous mappings. */
#define NCE_MMAP_OFFSET (NCE_BRK_OFFSET + 64UL * 1024 * 1024)
#define NCE_SYS_MAP_CODE 243
#define NCE_SYS_PUBLISH_CODE 244
#define NCE_MM_DIFF 0
#define NCE_MM_FULL 1
#define NCE_MM_LAZY 2
#define NCE_MM_DEMAND 3
#define NCE_FAULT_EXEC 2
#define NCE_FAULT_DIRECT 4
#define NCE_FAULT_TRY_PRESENT 8
#endif
