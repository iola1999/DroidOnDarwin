/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_NCE_WINDOW_H
#define _ASM_LKL_NCE_WINDOW_H

#include <linux/types.h>

/* Writers can hold Linux PTE locks. Guest leave must remain lock-free and
 * precede any Linux call that can wait for those locks. */
void nce_window_lock(void);
/* Stop guests before changing the installed window or its owning mm. */
void nce_window_stop(void);
void nce_window_unlock(void);
u64 nce_window_token(void);
bool nce_window_try_enter(u64 token);
bool nce_window_requested(void);
void nce_window_leave(void);
void nce_begin_user(void);
struct mm_struct;
void nce_smp_window_test(struct mm_struct *mm, unsigned long address);

#endif
