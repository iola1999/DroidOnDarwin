/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include "decode.h"
#include <random>

namespace
{
NceDirectState random_state(std::mt19937_64 &random)
{
    NceDirectState state{};
    for (auto &reg : state.x)
        reg = random();
    for (auto &vector : state.v) {
        vector[0] = random();
        vector[1] = random();
    }
    state.pc = CodeVA;
    state.sp = DataVA + 256;
    state.nzcv = random() & 0xf0000000;
    state.fpcr = (random() & 15) << 22;
    state.fpsr = random() & 0x0800009f;
    state.access_flags = NCE_DIRECT_UNALIGNED;
    state.exclusive_address = UINT64_MAX;
    return state;
}
void report(const char *name)
{
    std::printf("{\"test\":\"%s\",\"passed\":true}\n", name);
}
void literal_loads(Probe &probe, std::mt19937_64 &random)
{
    for (bool negative : {false, true}) {
        for (unsigned format = 0; format < 6; ++format) {
            for (unsigned rd = 0; rd < 32; ++rd) {
                std::array<uint32_t, 1024> code{};
                uint64_t pc = CodeVA + (negative ? 512 : 0);
                uint64_t source = CodeVA + 128;
                uint32_t imm = uint32_t((source - pc) / 4) & 0x7ffff;
                code[(pc - CodeVA) / 4] = 0x18000000 | ((format % 3) << 30) |
                                          (format >= 3 ? 1U << 26 : 0) | (imm << 5) | rd;
                for (unsigned i = 32; i < 36; ++i)
                    code[i] = random();
                probe.load(code.data(), sizeof(code));
                auto state = random_state(random);
                state.pc = pc;
                probe.compare(state, 1);
            }
        }
    }
    uint32_t code[] = {0x58000000 | (uint32_t((DataVA - CodeVA) / 4) << 5) | 18, 0};
    probe.load(code, sizeof(code));
    probe.readable = false;
    uint64_t fault = 0;
    auto initial = random_state(random), actual = initial;
    CHECK(probe.run(actual, 1000, &fault) == NCE_DIRECT_READ_FAULT);
    CHECK(fault == DataVA && !std::memcmp(&actual, &initial, sizeof(actual)));
    probe.readable = true;
    ++probe.epoch;
    probe.compare(initial, 1);
    // A literal can be aligned to four bytes while its Q access crosses a page.
    code[0] = 0x9c000000 | (uint32_t((DataVA + 4092 - CodeVA) / 4) << 5);
    probe.load(code, sizeof(code));
    actual = initial;
    auto counts = nce_direct_stats(probe.direct);
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
    CHECK(nce_direct_stats(probe.direct).cross_page_exits == counts.cross_page_exits + 1);
    CHECK(nce_direct_stats(probe.direct).alignment_exits == counts.alignment_exits);
    // Prefetch ignores inaccessible addresses and preserves every register.
    code[0] = 0xd8000000 | (0x3ffff << 5) | 31;
    probe.load(code, sizeof(code));
    probe.compare(initial, 1);
    report("literal_load_signed_vector_pc_and_permissions");
}
void structure_lanes(Probe &probe, std::mt19937_64 &random)
{
    for (auto &value : probe.data)
        value = random();
    constexpr unsigned bases[] = {0, 16, 17, 18, 26, 27, 28, 29, 30, 31};
    for (bool replicate : {false, true}) {
        for (unsigned bytes = 0; bytes < 4; ++bytes) {
            for (unsigned count = 1; count <= 4; ++count) {
                for (unsigned q = 0; q < 2; ++q) {
                    for (unsigned write = 0; write < (replicate ? 1U : 2U); ++write) {
                        for (unsigned mode = 0; mode < 3; ++mode) {
                            for (unsigned rn : bases) {
                                unsigned scale = replicate ? 3 : bytes == 3 ? 2
                                                                            : bytes;
                                unsigned size = replicate ? bytes : bytes == 0 ? 3
                                                                : bytes == 1   ? 2
                                                                : bytes == 3   ? 1
                                                                               : 0;
                                unsigned s = !replicate && bytes < 3;
                                unsigned rm = mode == 1 ? 31 : 18;
                                unsigned rt = (rn + count) & 31;
                                uint32_t word = 0x0d000000 | (q << 30) | (!write << 22) |
                                                (((count - 1) & 1) << 21) | (((count - 1) >> 1) << 13) |
                                                (scale << 14) | (s << 12) | (size << 10) | (rn << 5) | rt;
                                if (mode)
                                    word |= (1 << 23) | (rm << 16);
                                uint32_t code[] = {word, 0};
                                probe.load(code, sizeof(code));
                                auto state = random_state(random);
                                if (mode == 2)
                                    state.x[rm] = 96;
                                if (rn != 31)
                                    state.x[rn] = DataVA + 256;
                                probe.compare(state, 1);
                            }
                        }
                    }
                }
            }
        }
    }
    // A failed ST1 post-index must preserve its vector and address register.
    uint32_t code[] = {0x0d9f821f, 0};  // st1 {v31.s}[0], [x16], #4
    probe.load(code, sizeof(code));
    auto initial = random_state(random);
    initial.x[16] = DataVA;
    auto actual = initial;
    auto before = probe.data;
    probe.writable = false;
    uint64_t fault = 0;
    CHECK(probe.run(actual, 1000, &fault) == NCE_DIRECT_WRITE_FAULT);
    CHECK(fault == DataVA && !std::memcmp(&actual, &initial, sizeof(actual)) && before == probe.data);
    probe.writable = true;
    ++probe.epoch;
    probe.compare(initial, 1);
    initial.x[16] = DataVA + 4095;
    actual = initial;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
    code[0] = 0xf94003e0;  // ldr x0, [sp]
    probe.load(code, sizeof(code));
    initial.sp = DataVA + 1;
    actual = initial;
    auto counts = nce_direct_stats(probe.direct);
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
    CHECK(nce_direct_stats(probe.direct).alignment_exits == counts.alignment_exits + 1);
    CHECK(nce_direct_stats(probe.direct).cross_page_exits == counts.cross_page_exits);
    report("simd_lane_replicate_writeback_and_faults");
}
void fp_conversions(Probe &probe, std::mt19937_64 &random)
{
    constexpr uint32_t rounding[] = {0x1e200000, 0x1e210000, 0x1e240000, 0x1e250000,
                                     0x1e280000, 0x1e290000, 0x1e300000, 0x1e310000,
                                     0x1e380000, 0x1e390000};
    constexpr uint32_t fixed[] = {0x1e020000, 0x1e030000, 0x1e180000, 0x1e190000};
    constexpr uint64_t floating[] = {0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff800003fc00000),
                                     UINT64_C(0xbff80000bfc00000), UINT64_C(0x7ff800007fc00000),
                                     UINT64_C(0x7ff000007f800000), UINT64_C(0x43f000004f800000), 1};
    for (unsigned rd = 0; rd < 32; ++rd) {
        unsigned rn = (rd + 17) & 31;
        for (unsigned sf = 0; sf < 2; ++sf) {
            for (unsigned type = 0; type < 2; ++type) {
                for (uint32_t base : rounding) {
                    for (uint64_t bits : floating) {
                        uint32_t code[] = {base | (sf << 31) | (type << 22) | (rn << 5) | rd, 0};
                        probe.load(code, sizeof(code));
                        auto state = random_state(random);
                        state.v[rn][0] = bits;
                        probe.compare(state, 1);
                    }
                }
                for (uint32_t base : fixed) {
                    for (unsigned scale : {32U, 47U, 63U}) {
                        uint32_t code[] = {base | (sf << 31) | (type << 22) | (scale << 10) | (rn << 5) | rd, 0};
                        probe.load(code, sizeof(code));
                        probe.compare(random_state(random), 1);
                    }
                }
            }
        }
        for (unsigned half = 0; half < 2; ++half) {
            for (unsigned size = 0; size < 3; ++size) {
                for (uint32_t base : {0x0e201000U, 0x2e201000U, 0x0e203000U, 0x2e203000U}) {
                    uint32_t code[] = {base | (half << 30) | (size << 22) | (rn << 16) | (rd << 5) | rd, 0};
                    probe.load(code, sizeof(code));
                    probe.compare(random_state(random), 1);
                }
            }
            for (uint32_t base : {0x5e21d800U, 0x7e21d800U}) {
                uint32_t code[] = {base | (half << 22) | (rn << 5) | rd, 0};
                probe.load(code, sizeof(code));
                probe.compare(random_state(random), 1);
            }
        }
    }
    report("fp_fixed_rounding_and_widening");
}
void vector_tables(Probe &probe, std::mt19937_64 &random)
{
    auto original = probe.direct;
    auto config = probe.config;
    config.vector_table = true;
    auto enabled = nce_direct_create(config);
    config.vector_table = false;
    auto disabled = nce_direct_create(config);
    CHECK(enabled && disabled);
    for (unsigned q = 0; q < 2; ++q) {
        for (unsigned extension = 0; extension < 2; ++extension) {
            for (unsigned length = 1; length <= 4; ++length) {
                for (unsigned rn = 0; rn < 32; ++rn) {
                    for (unsigned alias = 0; alias < 4; ++alias) {
                        unsigned rm = alias == 3 ? (rn + length - 1) & 31 : (rn + 7) & 31;
                        unsigned rd = alias == 0 ? (rn + 9) & 31 : alias == 1 ? rn
                                                                              : rm;
                        uint32_t code[] = {0x0e000000 | (q << 30) | (rm << 16) |
                                               ((length - 1) << 13) | (extension << 12) | (rn << 5) | rd,
                                           0};
                        probe.load(code, sizeof(code));
                        for (unsigned pattern = 0; pattern < 4; ++pattern) {
                            auto initial = random_state(random);
                            auto *indices = reinterpret_cast<uint8_t *>(initial.v[rm]);
                            for (unsigned lane = 0; lane < 16; ++lane) {
                                // Include every table boundary and out-of-range indices,
                                // including when the index register is part of the table.
                                constexpr uint8_t boundaries[] = {0, 15, 16, 31, 32, 47, 48, 63, 64, 127, 128, 255};
                                indices[lane] = pattern < 3 ? boundaries[(lane + pattern * 4) % 12] : random();
                            }
                            probe.direct = disabled;
                            auto actual = initial;
                            CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
                            CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
                            probe.direct = enabled;
                            probe.compare(initial, 1);
                        }
                    }
                }
            }
        }
    }
    nce_direct_destroy(enabled);
    nce_direct_destroy(disabled);
    size_t fallback_bytes = 0;
    for (uint32_t first : {0U, 0x4e020324U}) {
        probe.direct = nce_direct_create(config);
        CHECK(probe.direct);
        std::array<uint32_t, 1024> code;
        code.fill(0xd503201f);  // NOPs must remain uncompiled after a disabled instruction.
        code[0] = first;
        probe.load(code.data(), sizeof(code));
        auto actual = random_state(random);
        CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
        size_t bytes = nce_direct_stats(probe.direct).code_bytes;
        if (!first)
            fallback_bytes = bytes;
        else
            CHECK(bytes == fallback_bytes);
        nce_direct_destroy(probe.direct);
    }
    probe.direct = original;
    for (uint32_t word : {0x8e000000U, 0x0e008000U, 0x0e800000U, 0x0e400000U})
        CHECK(nce_direct::decode(word) == nce_direct::Other);
    report("simd_table_alias_wrap_indices_and_setting");
}
}  // namespace

void direct_instruction_tests(Probe &probe)
{
    std::mt19937_64 random(0x494e5354);
    literal_loads(probe, random);
    structure_lanes(probe, random);
    fp_conversions(probe, random);
    vector_tables(probe, random);
    direct_simd_math_tests(probe);
    for (uint32_t word : {0xdc000000U, 0x0d010000U, 0x0d004400U, 0x0d008800U,
                          0x0d009400U, 0x0d00c000U, 0x0d40d000U,
                          0x1e020000U, 0x1e820000U, 0x9e060000U, 0x2ee01000U})
        CHECK(nce_direct::decode(word) == nce_direct::Other);
    report("new_instruction_reserved_encodings");
}
