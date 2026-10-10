/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

/* The Linux CPU writes these optional counters; the host log thread reads
 * individual fields atomically. A report is not a transactional snapshot. */
struct nce_mmu_profile {
    unsigned long switches;
    unsigned long scanned_pages;
    unsigned long present_faults;
    unsigned long linux_faults;
    unsigned long fast_faults;
    unsigned long fast_restored_pages;
    unsigned long restored_pages;
};
