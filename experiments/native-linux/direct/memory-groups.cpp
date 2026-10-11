/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "memory-groups.h"
#include <algorithm>

namespace nce_direct
{
int64_t memory_group_offset(uint32_t word, Kind kind, MemoryAccess access)
{
    return kind == Pair ? sign_extend((word >> 15) & 127, 7) * (1 << access.element_shift)
                        : ((word >> 10) & 4095) << access.element_shift;
}

MemoryGroup plan_memory_group(std::span<const uint32_t> words, unsigned page_bits)
{
    MemoryGroup group;
    int64_t end = 0;
    for (unsigned i = 0; i < std::min<size_t>(8, words.size()); ++i) {
        uint32_t word = words[i];
        Kind kind = decode(word);
        if (!((kind == Memory && (word & 0x3b000000) == 0x39000000) ||
              (kind == Pair && ((word >> 23) & 3) == 2)))
            break;
        auto access = memory_access(word, kind);
        unsigned base = (word >> 5) & 31;
        if (i && (base != group.base || access.write != group.write))
            break;
        bool vector = word & 0x04000000;
        if (!access.write && !vector && base != 31 &&
            ((word & 31) == base || (kind == Pair && ((word >> 10) & 31) == base)))
            break;
        int64_t offset = memory_group_offset(word, kind, access);
        int64_t first = i ? std::min(group.offset, offset) : offset;
        int64_t last = i ? std::max(end, offset + access.bytes) : offset + access.bytes;
        if (last - first > (INT64_C(1) << page_bits))
            break;
        bool encodable = true;
        for (unsigned j = 0; j <= i; ++j) {
            Kind item_kind = decode(words[j]);
            auto item = memory_access(words[j], item_kind);
            int64_t delta = memory_group_offset(words[j], item_kind, item) - first;
            unsigned scale = 1U << item.element_shift;
            encodable &= !(delta % scale) && delta / scale <= (item_kind == Pair ? 63 : 4095);
        }
        if (!encodable)
            break;
        group = {i + 1, base, unsigned(last - first), first, access.write};
        end = last;
    }
    return group.count >= 2 ? group : MemoryGroup{};
}
}  // namespace nce_direct
