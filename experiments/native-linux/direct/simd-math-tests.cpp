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
    auto compare = [&](uint32_t word, unsigned pattern) {
        CHECK(nce_direct::decode(word) == nce_direct::VectorMath);
        uint32_t code[] = {word, 0};
        probe.load(code, sizeof(code));
        NceDirectState initial{};
        for (auto &reg : initial.x)
            reg = random();
        initial.pc = CodeVA;
        initial.sp = DataVA + 256;
        initial.exclusive_address = UINT64_MAX;
        initial.nzcv = random() & 0xf0000000;
        initial.fpcr = (pattern & 15) << 22;
        initial.fpsr = random() & 0x0800009f;
        constexpr uint64_t values[] = {
            0, UINT64_C(0x8000000080000000), UINT64_C(0x3ff800003fc00000),
            UINT64_C(0xbff80000bfc00000), UINT64_C(0x7ff800007fc00000),
            UINT64_C(0x7ff000007f800000), UINT64_C(0x7ff000017f800001),
            UINT64_C(0x7fefffff7f7fffff), UINT64_C(0x0010000000800000),
            UINT64_C(0x0000000100000001)};
        for (unsigned r = 0; r < 32; ++r)
            for (unsigned half = 0; half < 2; ++half)
                initial.v[r][half] = pattern < 16 ? values[(r + half + pattern) % std::size(values)] : random();
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
    nce_direct_destroy(enabled);
    nce_direct_destroy(disabled);
    probe.direct = original;
    std::printf("{\"test\":\"simd_indexed_fmul_widening_subtract\",\"passed\":true,\"comparisons\":%u}\n", comparisons);
}
