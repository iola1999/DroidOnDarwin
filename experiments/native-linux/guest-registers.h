/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_GUEST_REGISTERS_H
#define NCE_GUEST_REGISTERS_H
/* Shared layout for Linux pt_regs and the first 800 bytes saved by entry.S.
 * Host execution state and guest TLS remain outside the signal-visible block. */
struct pt_regs {
    unsigned long x[31], sp, pc, nzcv, fpcr, fpsr;
    unsigned long vector[32][2];
};
#endif
