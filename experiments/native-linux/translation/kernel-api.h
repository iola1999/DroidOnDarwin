/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#define NCE_MM_TRANSLATED 4
#define NCE_TRANSLATION_CODE_WORDS 1024
#define NCE_TRANSLATION_MAX_CPUS 2

/* Writers stop affected guests before publishing invalidations. Each CPU
 * consumes its own pending ranges and bits before re-entering userspace. */
struct nce_translation_cpu {
    unsigned long mappings;
    unsigned long code;
    /* Pending virtual range belongs to the active mm; a switch covers all VA. */
    unsigned long tlb_start, tlb_end;
    unsigned long mm_switches, inactive_mapping_changes;
    /* The kernel retains these mappings throughout the guest interval. */
    unsigned long pgd;
    unsigned long flat;
    void *read_view;
    unsigned long code_pages[NCE_TRANSLATION_CODE_WORDS];
};

struct nce_translation_state {
    /* Guest limits and the host RAM alias are independent of RAM backing. */
    unsigned long guest_end, physical_base;
    unsigned long cpu_count;
    struct nce_translation_cpu cpus[NCE_TRANSLATION_MAX_CPUS];
    /* Host sets flat_enabled/RAM before boot; Linux owns each mm's table. */
    unsigned long flat_enabled, ram;
    /* SMP: one marker per physical page. Nonzero redirects guest writes to
     * the serialized fallback; Linux changes markers only with guests stopped. */
    unsigned long *code_guard;
    /* Startup option: ordinary data copies can overlap Direct execution. */
    unsigned long uaccess_code_only;
    unsigned long flat_live_bytes, flat_peak_bytes, flat_updates, flat_refresh_pages;
    unsigned long flat_read_changes, flat_write_changes;
    /* Host callbacks own opaque read aliases; Linux updates them with PTEs. */
    void *(*read_view_create)(unsigned long size);
    void (*read_view_update)(void *view, unsigned long offset, unsigned long source);
    void (*read_view_destroy)(void *view);
};
