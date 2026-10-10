/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

struct nce_schedule_profile {
    unsigned long samples;
    unsigned long same_mm_samples;
    unsigned long other_mm_samples;
    unsigned long same_mm_ready;
    unsigned long other_mm_ready;
    unsigned long kernel_or_host_ready;
    unsigned long max_same_mm_ready;
    unsigned long max_other_mm_ready;
    unsigned long interval_ns;
    unsigned long max_interval_ns;
    unsigned long collection_ns;
};

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
    unsigned long fault_retries;
    struct nce_schedule_profile schedule;
};

#ifdef __KERNEL__
void nce_sample_runnable(struct nce_schedule_profile *profile);
#endif
