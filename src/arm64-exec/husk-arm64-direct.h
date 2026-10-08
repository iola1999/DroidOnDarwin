/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "husk-arm64-exec.h"

struct HuskArm64Direct;
bool husk_direct_supported(uint32_t instruction);
HuskArm64Direct *husk_direct_create(size_t cache_bytes = 32 * 1024 * 1024);
struct HuskArm64CacheStats {
    uint64_t compiles, evictions, flushes;
    size_t bytes, pages;
};
HuskArm64CacheStats husk_direct_cache_stats(const HuskArm64Direct *);
void husk_direct_destroy(HuskArm64Direct *);
HuskArm64Exit husk_direct_run(HuskArm64Direct *, HuskArm64State *, uint64_t,
                            const void *, HuskArm64Memory, uint64_t);
