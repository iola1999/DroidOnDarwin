/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_NCE_CORE_H
#define _ASM_LKL_NCE_CORE_H

struct mm_struct;

/* The mm owns a cookie reference; each attached task owns another one. */
int nce_core_init_mm(struct mm_struct *mm);
void nce_core_destroy_mm(struct mm_struct *mm);
void nce_core_enter(void);

#endif
