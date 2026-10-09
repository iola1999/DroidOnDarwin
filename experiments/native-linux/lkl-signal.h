/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LKL_NCE_SIGNAL_H
#define _LKL_NCE_SIGNAL_H
#include <uapi/asm/signal.h>
#include <asm/sigcontext.h>
void nce_user_return(long syscall, long result, unsigned long original_x0);
void nce_user_fault(unsigned long address, int illegal_instruction);
#endif
