/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "decode.h"
#include <span>

namespace nce_direct
{
struct MemoryGroup {
    unsigned count = 0, base = 0, bytes = 0;
    int64_t offset = 0;
    bool write = false;
};
// The caller stops the span at branch targets and execution-budget boundaries.
// Zero count means the original per-instruction path must be used.
MemoryGroup plan_memory_group(std::span<const uint32_t>, unsigned page_bits);
int64_t memory_group_offset(uint32_t, Kind, MemoryAccess);
}  // namespace nce_direct
