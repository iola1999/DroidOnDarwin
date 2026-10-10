/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
typedef void (*NceSmpSetLayout)(unsigned long, unsigned long, unsigned long *,
                                unsigned long, void *, unsigned long, unsigned long);
void nce_smp_mmu_setup(NceSmpSetLayout set_layout);
void nce_smp_mmu_cleanup(void);
