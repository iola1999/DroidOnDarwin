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
    /* UINT64_MAX clears the guest reservation. Native reservations can fail
     * after any return to the host, including a mapping refill. */
    uint64_t exclusive_address, exclusive_value;
} NceDirectState;

typedef struct {
    /* LKL 16 KiB page tables; directory entries contain host pointers.
     * Leaf physical addresses in [physical_base, physical_base + ram_size)
     * correspond to the contiguous host RAM range starting at ram.
     * The caller keeps all tables and RAM alive throughout a run. */
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
    /* Optional Linux-maintained table covering the first 4 GiB from user_base.
     * Higher addresses use page_table. Requires page_table and excludes refill_reads. Nonzero
     * entries must be aligned pointers inside its RAM; only Linux updates them
     * while affected CPUs are outside run. */
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
    /* Optional read-only 4 GiB alias range matching flat_pages. Requires
     * installed read-fault handling. Permissions/backing remain stable during
     * run; the caller updates aliases with every readable PTE change. */
    const void *read_view;
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
    uint64_t memory_groups, memory_group_instructions;
    uint64_t read_view_sites, read_view_faults;
    size_t code_bytes, code_capacity, pages;
} NceDirectStats;

typedef struct {
    size_t cache_bytes;
    /* Power-of-two entry counts, from 256 through 65536. */
    unsigned mapping_entries, route_entries;
    bool selective_writes;
    bool reuse_code_checks;
    /* Ordinary RAM only. The caller retains both mappings and excludes writes
     * to executable backing while the instruction is completed. */
    bool cross_page;
    /* Validate route generations instead of clearing every entry on return. */
    bool route_epochs;
    /* Keep routes across calls with stable mapping/code epochs. Requires
     * route_epochs; reuse applies only to direct page-table/flat memory. */
    bool retain_routes;
    /* Share one translation for adjacent accesses to the same RAM page. */
    bool memory_groups;
    /* Use the host exclusive monitor. The default LDAR/CAS approximation
     * permits ABA and is retained for existing single-CPU comparisons.
     * If a host return interrupts a reservation, the next exclusive load in
     * that run returns UNSUPPORTED so a caller can execute its retry elsewhere. */
    bool native_exclusive;
    /* TBL/TBX execute with the saved guest SIMD registers when enabled. */
    bool vector_table;
    /* Validated indexed FMUL and widening subtraction use host SIMD. */
    bool simd_math;
} NceDirectConfig;

/* One sequential caller per engine. Code cache is 2..256 MiB, 16 KiB aligned.
 * The executable-memory model is selected at build time. */
NceDirect *nce_direct_create(NceDirectConfig);
void nce_direct_destroy(NceDirect *);
/* Returns "map-jit" or "dual" for the linked engine. */
const char *nce_direct_code_memory(void);
/* Dual variant only. Check prepared memory before any guest code is compiled.
 * The caller serializes all code checks and stops all users of this engine. */
int nce_direct_check_code(NceDirect *engine);
/* Request from another thread; consumed at the next block boundary. The owner
 * must exclude engine destruction. An unprepared run clears earlier requests. */
void nce_direct_request_stop(NceDirect *);
/* The owning thread calls this before registering guest execution with an
 * external writer. The next run preserves stop requests received afterwards.
 * Calls without preparation continue to clear old requests on entry. */
void nce_direct_prepare_run(NceDirect *);
/* Optional shared RAM code markers for concurrent executors using page_table
 * without refill_reads. One 64-bit word per 16 KiB RAM page, value 0 or 1.
 * The caller updates markers only while all affected runs are stopped. */
void nce_direct_set_code_guard(NceDirect *, const uint64_t *pages, size_t count);
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
/* Process-wide signal ownership, installed before other handlers which may
 * forward read faults. Remove after those handlers and all guest executions.
 * Only one read-view execution may be active at a time; unrelated faults are
 * forwarded to the previous handlers. Returns 0 or -1 with errno. */
int nce_direct_install_read_faults(void);
int nce_direct_remove_read_faults(void);
#ifdef __cplusplus
}
#endif
