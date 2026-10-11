/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#define NCE_MM_TRANSLATED 4
#define NCE_TRANSLATION_CODE_WORDS 1024

/* The single Linux CPU writes these counters while the engine is stopped.
 * The host reads them before re-entering translated userspace. */
struct nce_translation_state {
    unsigned long mappings;
    unsigned long code;
    /* Pending virtual range belongs to the active mm; a switch covers all VA. */
    unsigned long tlb_start, tlb_end;
    unsigned long mm_switches, inactive_mapping_changes;
    /* Current mm's PGD, valid while the sole Linux CPU executes userspace. */
    unsigned long pgd;
    /* Host sets flat_enabled/RAM before boot; Linux owns each mm's table. */
    unsigned long flat_enabled, ram, flat;
    unsigned long flat_live_bytes, flat_peak_bytes, flat_updates, flat_refresh_pages;
    unsigned long flat_read_changes, flat_write_changes;
    /* Host callbacks own opaque read aliases; Linux updates them with PTEs. */
    void *read_view;
    void *(*read_view_create)(unsigned long size);
    void (*read_view_update)(void *view, unsigned long offset, unsigned long source);
    void (*read_view_destroy)(void *view);
    /* One bit per 16 KiB RAM page. The host consumes bits before execution. */
    unsigned long code_pages[NCE_TRANSLATION_CODE_WORDS];
};
