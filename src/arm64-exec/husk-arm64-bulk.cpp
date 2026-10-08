/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"
#include <algorithm>
#include <array>
#include <cstring>

bool husk_arm64_bulk(uint64_t destination, uint64_t source, uint64_t length,
                     unsigned operation, HuskArm64Memory memory)
{
    if (operation > HUSK_BULK_SET || length > HUSK_BULK_LIMIT) return false;
    if (!length) return true;
    if (!memory.map_page || destination > UINT64_MAX - length ||
        (operation != HUSK_BULK_SET && source > UINT64_MAX - length)) return false;
    struct Span { unsigned char *data; size_t size; };
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
            if (!host) return false;
            spans[count++] = {host + offset, size};
            address += size;
            remaining -= size;
        }
        return true;
    };
    /* Preflight every page before modifying RAM. Refusal lets the guest's
     * ordinary implementation produce its own fault and partial-copy state. */
    if (operation != HUSK_BULK_SET && !validate(source, false, reads, read_count)) return false;
    if (!validate(destination, true, writes, write_count)) return false;
    if (operation == HUSK_BULK_SET) {
        for (size_t i = 0; i < write_count; ++i)
            std::memset(writes[i].data, source & 255, writes[i].size);
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
