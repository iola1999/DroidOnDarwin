/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "husk-arm64-exec.h"
#include <array>
#include <cstdint>
#include <vector>

/* Each entry covers 1 GiB. Lower and upper 48-bit guest regions have separate
 * indices, so equal low address bits cannot alias. Unused entries share a
 * protected reservation; physical RAM is never copied. */
class HuskArm64Fastmem
{
   public:
    static constexpr unsigned segment_bits = 30, address_bits = 48;
    static constexpr uint64_t segment_size = UINT64_C(1) << segment_bits;
    static constexpr size_t segment_count = 1U << (address_bits + 1 - segment_bits);
    static bool compact_address(uint64_t &address, bool ignore_top_byte)
    {
        if (ignore_top_byte)
            address &= UINT64_C(0x00ffffffffffffff);
        uint64_t upper = address >> address_bits;
        if (upper != 0 && upper != 0xffff)
            return false;
        address = (address & ((UINT64_C(1) << address_bits) - 1)) |
                  (uint64_t(upper != 0) << address_bits);
        return true;
    }

    HuskArm64Fastmem();
    ~HuskArm64Fastmem();
    HuskArm64Fastmem(const HuskArm64Fastmem &) = delete;
    HuskArm64Fastmem &operator=(const HuskArm64Fastmem &) = delete;

    bool available() const
    {
        return guard != 0;
    }
    const uintptr_t *bases(bool write) const
    {
        return windows[write].bases.data();
    }
    bool map(uint64_t address, bool write, HuskArm64Memory memory, bool ignore_top_byte);
    /* Reset whenever the owner's mapping generation changes. */
    void reset();
    void protect_code(const void *host_page);

   private:
    bool map_validated(uint64_t address, bool write, HuskArm64Memory memory, bool ignore_top_byte);
    struct Window {
        std::array<uintptr_t, segment_count> bases{};
        struct Page {
            uint64_t guest;
            uintptr_t host;
        };
        std::vector<Page> mapped;
    };
    void revoke(Window &window, uint64_t page);
    std::array<Window, 2> windows;
    uintptr_t guard = 0;
    size_t host_page = 0;
    uint64_t attempts = 0, installed = 0, revoked = 0;
    bool profile = false;
    const char *rejection = "none";
};
