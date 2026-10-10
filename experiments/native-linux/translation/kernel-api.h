/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#define NCE_MM_TRANSLATED 4
#define NCE_TRANSLATION_CODE_WORDS 1024

/* The single Linux CPU writes these counters while the engine is stopped.
 * The host reads them before re-entering translated userspace. */
struct nce_translation_state {
    unsigned long mappings;
    unsigned long code;
    /* One bit per 16 KiB RAM page. The host consumes bits before execution. */
    unsigned long code_pages[NCE_TRANSLATION_CODE_WORDS];
};
