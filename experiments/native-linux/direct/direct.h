/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "page-table-layout.h"
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
    /* LKL 16 KiB page tables; directory entries contain host pointers.
     * Leaf physical addresses in [physical_base, physical_base + ram_size)
     * correspond to the contiguous host RAM range starting at ram.
     * The sole Linux CPU keeps all tables and RAM alive throughout a run. */
    const uint64_t *root;
    uint64_t user_base, user_size, physical_base;
    void *ram;
    uint64_t ram_size;
} NceDirectPageTable;

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
    /* Optional direct table walk. Requires 16 KiB data pages and aligned RAM.
     * map_code must return pointers into this RAM range. */
    const NceDirectPageTable *page_table;
    /* With page_table, use the table walker only to refill read-cache misses.
     * Writes keep map_page and the writable-code-alias checks. */
    bool refill_reads;
    /* Optional Linux-maintained table, one entry per 16 KiB page in the 4 GiB
     * user range. Requires page_table and excludes refill_reads. Nonzero
     * entries must be aligned pointers inside its RAM; only Linux updates them
     * while the sole CPU is outside run. */
    const struct nce_direct_flat_page *flat_pages;
    /* Optional physical code-page version. Zero requests content comparison.
     * A nonzero version must change after any write or backing replacement,
     * never repeat for that RAM address during the engine's lifetime, and
     * remain stable throughout run. Execution permission is checked separately. */
    uint64_t (*code_version)(void *, const void *ram_address);
    /* Nonzero permits route reuse with mapping_epoch. Change after every
     * mutation/replacement of previously versioned code, including writes via
     * aliases. Values must not repeat. Zero keeps per-call validation. */
    uint64_t code_epoch;
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
    uint64_t code_validation_hits;
    uint64_t pte_read_refills;
    uint64_t code_content_checks, code_version_hits;
    uint64_t alignment_exits, cross_page_exits;
    uint64_t cross_page_completed, cross_page_fallbacks, cross_page_faults, cross_page_bytes;
    uint64_t route_invalidations, route_cleared_entries, branch_exits, route_reused_runs;
    size_t code_bytes, code_capacity, pages;
} NceDirectStats;

typedef struct {
    size_t cache_bytes;
    /* Power-of-two entry counts, from 256 through 65536. */
    unsigned mapping_entries, route_entries;
    bool selective_writes;
    bool reuse_code_checks;
    /* Ordinary RAM only, with serialized guest execution. */
    bool cross_page;
    /* Validate route generations instead of clearing every entry on return. */
    bool route_epochs;
    /* Keep routes across calls with stable mapping/code epochs. Requires
     * route_epochs; reuse applies only to direct page-table/flat memory. */
    bool retain_routes;
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
 * Cross-page ordinary RAM accesses can be enabled at creation. Otherwise they
 * return UNSUPPORTED. Ordered and exclusive accesses keep their alignment checks.
 * Writes to visited code return WRITE_FAULT before the store. With retained
 * routes this includes targets visited during earlier calls in the same
 * validation interval.
 * fault_address is meaningful for FETCH/READ/WRITE_FAULT only. */
NceDirectExit nce_direct_run(NceDirect *, NceDirectState *, NceDirectMemory,
                             uint64_t budget, uint64_t *fault_address);
NceDirectStats nce_direct_stats(const NceDirect *);
#ifdef __cplusplus
}
#endif
