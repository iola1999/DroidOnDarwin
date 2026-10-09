/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"
#include <algorithm>
#include <array>
#include <cstring>

static bool bulk(uint64_t destination, uint64_t source, uint64_t length,
                 unsigned operation, HuskArm64Memory memory, bool mops)
{
    if (operation > HUSK_BULK_SET || length > HUSK_BULK_LIMIT)
        return false;
    if (!length)
        return true;
    if (!memory.map_page || destination > UINT64_MAX - length ||
        (operation != HUSK_BULK_SET && source > UINT64_MAX - length))
        return false;
    struct Span {
        unsigned char *data;
        size_t size;
        uint64_t guest;
    };
    using Spans = std::array<Span, HUSK_BULK_LIMIT / 4096 + 1>;
    Spans reads, writes;
    size_t read_count = 0, write_count = 0;
    auto validate = [&](uint64_t address, bool write, Spans &spans, size_t &count) {
        uint64_t remaining = length;
        while (remaining) {
            size_t offset = address & 4095;
            size_t size = std::min(remaining, uint64_t(4096 - offset));
            auto *host = static_cast<unsigned char *>(
                memory.map_page(memory.opaque, address & ~UINT64_C(4095), write));
            if (!host)
                return false;
            spans[count++] = {host + offset, size, address};
            address += size;
            remaining -= size;
        }
        return true;
    };
    /* Preflight every page before modifying RAM. Refusal lets the guest's
     * ordinary implementation produce its own fault and partial-copy state. */
    if (operation != HUSK_BULK_SET && !validate(source, false, reads, read_count))
        return false;
    if (!validate(destination, true, writes, write_count))
        return false;
    if (mops && operation != HUSK_BULK_SET) {
        // QEMU chooses copy direction from virtual addresses. Distinct virtual
        // ranges aliasing RAM must retain its page-by-page copy order.
        for (size_t r = 0; r < read_count; ++r)
            for (size_t w = 0; w < write_count; ++w) {
                auto from = uintptr_t(reads[r].data), to = uintptr_t(writes[w].data);
                if (from < to + writes[w].size && to < from + reads[r].size &&
                    from - reads[r].guest != to - writes[w].guest)
                    return false;
            }
    }
    if (operation == HUSK_BULK_SET) {
        for (size_t i = 0; i < write_count; ++i)
            std::memset(writes[i].data, source & 255, writes[i].size);
    } else if (read_count == 1 && write_count == 1) {
        std::memmove(writes[0].data, reads[0].data, length);
    } else {
        /* Distinct guest virtual pages can alias the same RAM. Gather first
         * so memmove also preserves overlap across fragmented physical pages. */
        std::array<unsigned char, HUSK_BULK_LIMIT> temporary;
        size_t offset = 0;
        for (size_t i = 0; i < read_count; ++i) {
            std::memcpy(temporary.data() + offset, reads[i].data, reads[i].size);
            offset += reads[i].size;
        }
        offset = 0;
        for (size_t i = 0; i < write_count; ++i) {
            std::memcpy(writes[i].data, temporary.data() + offset, writes[i].size);
            offset += writes[i].size;
        }
    }
    return true;
}

bool husk_arm64_bulk(uint64_t destination, uint64_t source, uint64_t length,
                     unsigned operation, HuskArm64Memory memory)
{
    return bulk(destination, source, length, operation, memory, false);
}

bool husk_arm64_mops(HuskArm64State *state, const uint32_t code[3], HuskArm64Memory memory)
{
    uint32_t word = code[0];
    if (!husk_arm64_mops_sequence(code) || (state->access_flags & HUSK_BTYPE_MASK) ||
        state->pc > UINT64_MAX - 12)
        return false;
    bool set = (word & 0xffe0fc00) == 0x19c00400;
    unsigned rd = word & 31, rn = (word >> 5) & 31, rs = (word >> 16) & 31;
    uint64_t to = state->x[rd], from = rs == 31 ? 0 : state->x[rs], length = state->x[rn];
    if (state->access_flags & 8) {
        to &= UINT64_C(0x00ffffffffffffff);
        if (!set)
            from &= UINT64_C(0x00ffffffffffffff);
    }
    if ((to >> 48) || (!set && (from >> 48)) ||
        !bulk(to, from, length, set ? HUSK_BULK_SET : HUSK_BULK_MOVE, memory, true))
        return false;
    bool reverse = !set && from < to && to - from < length;
    if (!reverse) {
        state->x[rd] += length;
        if (!set)
            state->x[rs] += length;
    }
    state->x[rn] = 0;
    state->nzcv = 0;
    state->pc += 12;
    return true;
}
