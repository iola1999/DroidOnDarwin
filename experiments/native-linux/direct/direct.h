/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct NceDirect NceDirect;
enum {
    NCE_DIRECT_UNALIGNED = 1,
    NCE_DIRECT_LSE = 4,
    NCE_DIRECT_CRC = 64,
    NCE_DIRECT_AES = 256,
    NCE_DIRECT_SHA1 = 512,
    NCE_DIRECT_SHA256 = 1024,
    NCE_DIRECT_PAC_HINT_NOP = 2048,
    NCE_DIRECT_DC_ZVA64 = 4096
};
typedef struct {
    uint64_t x[31], sp, pc;
    uint32_t nzcv, fpcr, fpsr, access_flags;
    uint64_t tpidr_el0, v[32][2];
    /* UINT64_MAX clears the software reservation. Single Linux CPU only. */
    uint64_t exclusive_address, exclusive_value;
} NceDirectState;

typedef struct {
    void *opaque;
    /* Validate a complete data page and return its aligned RAM pointer.
     * Mappings and code bytes remain valid until run returns. Callbacks may
     * reject a page, but must not change the address space or unwind the call. */
    void *(*map_page)(void *, uint64_t address, bool write);
    const void *(*map_code)(void *, uint64_t address);
    /* Nonzero permits read mapping reuse. Change after every address-space,
     * permission or backing change. Zero revalidates each run. */
    uint64_t mapping_epoch;
    /* Data page size is 1 << page_bits, with 0 selecting 12. Supports 12/14.
     * Code callbacks always validate 4 KiB. */
    unsigned page_bits;
} NceDirectMemory;

typedef enum {
    NCE_DIRECT_BUDGET,
    NCE_DIRECT_UNSUPPORTED,
    NCE_DIRECT_FETCH_FAULT,
    NCE_DIRECT_READ_FAULT,
    NCE_DIRECT_WRITE_FAULT,
    NCE_DIRECT_ERROR
} NceDirectExit;
typedef struct {
    uint64_t compiles, flushes, code_changes, mapping_misses, code_lookups;
    uint64_t read_mapping_misses, write_mapping_misses, write_invalidations;
    size_t code_bytes, code_capacity, pages;
} NceDirectStats;

typedef struct {
    size_t cache_bytes;
    /* Power-of-two entry counts, from 256 through 65536. */
    unsigned mapping_entries, route_entries;
    bool selective_writes;
} NceDirectConfig;

/* One sequential caller per engine. Code cache is 2..256 MiB, 16 KiB aligned.
 * This independent prototype currently prepares MAP_JIT memory on macOS. */
NceDirect *nce_direct_create(NceDirectConfig);
void nce_direct_destroy(NceDirect *);
/* Request from another thread; consumed at the next block boundary. The owner
 * must exclude engine destruction. A new run clears earlier requests. */
void nce_direct_request_stop(NceDirect *);
/* Budget is a soft instruction limit, checked at block/indirect-branch entries.
 * Fault/unsupported returns preserve the complete state at the pending PC.
 * Cross-page memory operations return UNSUPPORTED for an external executor.
 * Writes to code visited during this call return WRITE_FAULT before the store.
 * fault_address is meaningful for FETCH/READ/WRITE_FAULT only. */
NceDirectExit nce_direct_run(NceDirect *, NceDirectState *, NceDirectMemory,
                             uint64_t budget, uint64_t *fault_address);
NceDirectStats nce_direct_stats(const NceDirect *);
#ifdef __cplusplus
}
#endif
