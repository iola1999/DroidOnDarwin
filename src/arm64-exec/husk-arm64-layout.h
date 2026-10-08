/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_ARM64_LAYOUT_H
#define HUSK_ARM64_LAYOUT_H

/* Shared with the assembly entry/exit; C++ verifies each offset. */
#define HUSK_PC 256
#define HUSK_NZCV 264
#define HUSK_FPCR 268
#define HUSK_FPSR 272
#define HUSK_ACCESS_FLAGS 276
#define HUSK_TPIDR 280
#define HUSK_VECTORS 288
#define HUSK_EXCLUSIVE_ADDR 800
#define HUSK_EXCLUSIVE_VALUE 808
#define HUSK_BUDGET 816
#define HUSK_ADDRESS 824
#define HUSK_REASON 832
#define HUSK_LOOP_BASE 840
#define HUSK_CODE_ROUTES 848
#define HUSK_CALL_ROUTES 856
#define HUSK_READ_WINDOWS 864
#define HUSK_WRITE_WINDOWS 872
#define HUSK_PAUTH_CACHE 880
#define HUSK_NATIVE_PAUTH 928
#define HUSK_KERNEL_READS 936
#define HUSK_FPCR_MASK 0x03c00000

#endif
