/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct NceCodeWatch NceCodeWatch;
typedef struct {
    uint64_t queries, protections, write_faults, invalidations, protection_failures, excluded_pages;
} NceCodeWatchStats;

/* One watch per process, two shared RW mappings of the same RAM. All addresses
 * and size must use the 16 KiB host page size. Create/destroy, version queries
 * and invalidation require quiescent writers. Concurrent writes are allowed
 * between queries; each write fault restores both aliases before retrying.
 * The owner must destroy the watch before either mapping is removed. */
NceCodeWatch *nce_code_watch_create(void *ram, void *alias, size_t size);
void nce_code_watch_destroy(NceCodeWatch *);
/* Returns a nonzero stable version for the containing RAM page, or zero when
 * protection failed. A version cannot survive mutation or range replacement. */
uint64_t nce_code_watch_version(NceCodeWatch *, const void *ram_address);
/* Read with quiescent writers. Changes on a tracked write or invalidation;
 * version() returning zero still requires per-call content checks. */
uint64_t nce_code_watch_epoch(const NceCodeWatch *);
/* Call before replacing backing storage; restores write access for the range. */
int nce_code_watch_invalidate(NceCodeWatch *, size_t offset, size_t size);
/* Additional writable aliases require content comparison for these pages for
 * the rest of this watch's lifetime. Call before publishing the new alias. */
int nce_code_watch_exclude(NceCodeWatch *, size_t offset, size_t size);
NceCodeWatchStats nce_code_watch_stats(const NceCodeWatch *);
#ifdef __cplusplus
}
#endif
