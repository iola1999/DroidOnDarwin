/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_BARRIER_H
#define _ASM_LKL_BARRIER_H

#define __mb() __asm__ __volatile__("dmb ish" ::: "memory")
#define __rmb() __asm__ __volatile__("dmb ishld" ::: "memory")
#define __wmb() __asm__ __volatile__("dmb ishst" ::: "memory")
#include <asm-generic/barrier.h>

#endif
