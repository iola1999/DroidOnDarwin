/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include "decode.h"
#include <random>

void direct_simd_math_tests(Probe &probe)
{
    auto original = probe.direct;
    auto config = probe.config;
    config.simd_math = true;
    auto enabled = nce_direct_create(config);
    config.simd_math = false;
    auto disabled = nce_direct_create(config);
    CHECK(enabled && disabled);
    std::mt19937_64 random(0x53494d444d415448);
    unsigned comparisons = 0;
    auto compare = [&](uint32_t word, unsigned pattern, unsigned nzcv = 16) {
        CHECK(nce_direct::decode(word) == nce_direct::VectorMath);
        uint32_t code[] = {word, 0};
        probe.load(code, sizeof(code));
        NceDirectState initial{};
        for (auto &reg : initial.x)
            reg = random();
        initial.pc = CodeVA;
        initial.sp = DataVA + 256;
        initial.exclusive_address = UINT64_MAX;
        initial.nzcv = nzcv < 16 ? nzcv << 28 : random() & 0xf0000000;
        initial.fpcr = (pattern & 15) << 22;
        initial.fpsr = random() & 0x0800009f;
        constexpr uint64_t doubles[] = {
            0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff8000000000000),
            UINT64_C(0xbff8000000000000), UINT64_C(0x7ff8000000000000),
            UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff0000000000001),
            UINT64_C(0x7fefffffffffffff), UINT64_C(0x0010000000000000), 1};
        constexpr uint32_t singles[] = {0, 0x80000000, 0x3fc00000, 0xbfc00000, 0x7fc00000,
                                        0x7f800000, 0x7f800001, 0x7f7fffff, 0x00800000, 1};
        for (unsigned r = 0; r < 32; ++r)
            for (unsigned half = 0; half < 2; ++half) {
                unsigned index = (r + half + pattern) % std::size(doubles);
                uint64_t value = word & (1U << 22) ? doubles[index]
                                                   : uint64_t(singles[index]) | (uint64_t(singles[(index + 3) % std::size(singles)]) << 32);
                initial.v[r][half] = pattern < 16 ? value : random();
            }
        probe.direct = disabled;
        auto actual = initial;
        CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
        CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
        probe.direct = enabled;
        probe.compare(initial, 1);
        ++comparisons;
    };
    // Include all source and destination registers, aliases, and every lane
    // for each scalar/vector width. FP modes cover rounding, FZ, and DN.
    for (unsigned form = 0; form < 3; ++form) {
        for (unsigned size = 2; size <= 3; ++size) {
            if (form == 0 && size == 3)
                continue;
            for (unsigned lane = 0; lane < (size == 2 ? 4U : 2U); ++lane) {
                for (unsigned rn = 0; rn < 32; ++rn) {
                    for (unsigned alias = 0; alias < 3; ++alias) {
                        unsigned rm = (rn + 17) & 31;
                        unsigned rd = alias == 0 ? rn : alias == 1 ? rm
                                                                   : (rn + 9) & 31;
                        unsigned h = size == 2 ? lane >> 1 : lane, l = size == 2 ? lane & 1 : 0;
                        uint32_t word = (form == 2 ? 0x5f009000U : 0x0f009000U | (form << 30)) |
                                        (size << 22) | (l << 21) | (rm << 16) | (h << 11) | (rn << 5) | rd;
                        for (unsigned pattern = 0; pattern < 18; ++pattern)
                            compare(word, pattern);
                    }
                }
            }
        }
    }
    for (unsigned q = 0; q < 2; ++q)
        for (unsigned u = 0; u < 2; ++u)
            for (unsigned size = 0; size < 3; ++size)
                for (unsigned rn = 0; rn < 32; ++rn)
                    for (unsigned alias = 0; alias < 3; ++alias) {
                        unsigned rm = (rn + 17) & 31, rd = alias == 0 ? rn : alias == 1 ? rm
                                                                                        : (rn + 9) & 31;
                        compare(0x0e202000U | (q << 30) | (u << 29) | (size << 22) | (rm << 16) | (rn << 5) | rd, 17);
                    }
    // Reserved sizes, scalar Q=0, FP16, and FP64's reserved L=1 stay outside
    // this candidate. No unvalidated encoding may reach the host instruction.
    for (uint32_t word : {0x0f409000U, 0x0fc09000U, 0x4fe09000U, 0x1f809000U,
                          0x5fe09000U, 0x0f009000U, 0x5f009000U, 0x0ee02000U, 0x6ee02000U})
        CHECK(nce_direct::decode(word) == nce_direct::Other);

    for (unsigned size = 0; size < 4; ++size)
        for (unsigned lane = 0; lane < (16U >> size); ++lane)
            for (unsigned rn = 0; rn < 32; ++rn)
                for (unsigned rd : {rn, (rn + 9) & 31})
                    compare(0x5e000400U | (((lane << (size + 1)) | (1U << size)) << 16) | (rn << 5) | rd, 17);
    for (uint32_t base : {0x0e20e400U, 0x2e20e400U, 0x2ea0e400U, 0x2ea0d400U,
                          0x5e20e400U, 0x7e20e400U, 0x7ea0e400U, 0x7ea0d400U,
                          0x0e21d800U, 0x2e21d800U, 0x0ea0f800U, 0x2ea0f800U}) {
        bool scalar = base & (1U << 28);
        bool unary = (base & 0xfc00) == 0xd800 || (base & 0xfc00) == 0xf800;
        for (unsigned width = 0; width < 2; ++width)
            for (unsigned q = scalar || width ? 1 : 0; q < 2; ++q)
                for (unsigned rn = 0; rn < 32; ++rn)
                    for (unsigned alias = 0; alias < 3; ++alias) {
                        unsigned rm = (rn + 17) & 31, rd = alias == 0 ? rn : alias == 1 ? rm
                                                                                        : (rn + 9) & 31;
                        uint32_t word = base | (q << 30) | (width << 22) | (unary ? 0 : rm << 16) | (rn << 5) | rd;
                        for (unsigned pattern = 0; pattern < 18; ++pattern)
                            compare(word, pattern);
                    }
    }
    // Conditional compare must consume the guest flags, including the false
    // condition path which substitutes the immediate NZCV without comparing.
    for (unsigned width = 0; width < 2; ++width)
        for (unsigned signaling = 0; signaling < 2; ++signaling)
            for (unsigned condition = 0; condition < 16; ++condition)
                for (unsigned flags = 0; flags < 16; ++flags)
                    for (unsigned pattern = 0; pattern < 18; ++pattern) {
                        unsigned rn = (pattern + condition) & 31, rm = (rn + flags) & 31;
                        compare(0x1e200400U | (width << 22) | (rm << 16) | (condition << 12) | (rn << 5) | (signaling << 4) | ((flags + 7) & 15), pattern, flags);
                    }
    for (uint32_t word : {0x5e000400U, 0x5e100400U, 0x7e010400U, 0x5e010c00U,
                          0x0e60e400U, 0x2e60e400U, 0x2ee0e400U, 0x2ee0d400U,
                          0x0e61d800U, 0x2e61d800U, 0x0ee0f800U, 0x2ee0f800U,
                          0x1ea00400U, 0x1ee00400U})
        CHECK(nce_direct::decode(word) == nce_direct::Other);
    nce_direct_destroy(enabled);
    nce_direct_destroy(disabled);
    probe.direct = original;
    std::printf("{\"test\":\"simd_math\",\"passed\":true,\"comparisons\":%u}\n", comparisons);
}
