/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "husk-arm64-exec.h"

struct HuskArm64Direct;
bool husk_direct_supported(uint32_t instruction);
// Zero reads HUSK_ARM64_CODE_CACHE_MIB (default 32). iOS also requires that
// the prewarmed JIT partition can hold the requested capacity.
HuskArm64Direct *husk_direct_create(size_t cache_bytes = 0);
struct HuskArm64CacheStats {
    uint64_t compiles, evictions, flushes, shared_hits;
    size_t bytes, pages;
};
HuskArm64CacheStats husk_direct_cache_stats(const HuskArm64Direct *);
void husk_direct_destroy(HuskArm64Direct *);
HuskArm64Exit husk_direct_run(HuskArm64Direct *, HuskArm64State *, uint64_t,
                            const void *, HuskArm64Memory, uint64_t);
