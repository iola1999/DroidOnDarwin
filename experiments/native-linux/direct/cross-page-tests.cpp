/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include "decode.h"
#include <random>

namespace
{
struct Crossing {
    Probe &probe;
    unsigned bits, page_bytes;
    alignas(16384) std::array<unsigned char, 65536> ram{};
    std::array<bool, 4> readable{true, true, true, true}, writable{true, true, true, true};
    bool alias_code = false;
    uint64_t epoch = 1;
    NceDirect *engine;
    Crossing(Probe &owner, unsigned page_bits) : probe(owner), bits(page_bits), page_bytes(1U << bits), engine(nce_direct_create({16 * 1024 * 1024, 256, 256, true, false, true, owner.config.route_epochs, false}))
    {
        CHECK(engine);
        std::mt19937_64 random(0x43524f5353);
        for (auto &value : ram)
            value = random();
    }
    ~Crossing()
    {
        nce_direct_destroy(engine);
    }
    unsigned char *physical(unsigned page)
    {
        constexpr unsigned order[] = {2, 0, 3, 1};
        return ram.data() + order[page] * page_bytes;
    }
    static void *map(void *opaque, uint64_t address, bool write)
    {
        auto &self = *static_cast<Crossing *>(opaque);
        if (address < DataVA || address >= DataVA + 4 * self.page_bytes)
            return Probe::Map(&self.probe, address, write);
        unsigned page = unsigned(address - DataVA) >> self.bits;
        if (!(write ? self.writable[page] : self.readable[page]))
            return nullptr;
        if (self.alias_code && page == 1)
            return self.probe.code.data();
        return self.physical(page);
    }
    static const void *code(void *opaque, uint64_t address)
    {
        return Probe::Code(&static_cast<Crossing *>(opaque)->probe, address);
    }
    void load(uint32_t word, uint64_t pc = CodeVA)
    {
        std::array<uint32_t, 2048> code{};
        unsigned index = unsigned(pc - CodeVA) / 4;
        code[index] = word;
        code[index + 1] = 0x910004a5;  // add x5, x5, #1 after the boundary access
        probe.load(code.data(), sizeof(code));
        ++epoch;
    }
    NceDirectExit run(NceDirectState &state, uint64_t &fault)
    {
        NceDirectMemory memory{this, map, code, epoch, bits, nullptr, false, nullptr, nullptr, 0};
        return nce_direct_run(engine, &state, memory, 1000, &fault);
    }
    void compare(uint32_t word, NceDirectState initial)
    {
        load(word, initial.pc);
        for (unsigned page = 0; page < 4; ++page)
            std::memcpy(reinterpret_cast<unsigned char *>(probe.reference.data()) + page * page_bytes, physical(page), page_bytes);
        auto expected = initial, actual = initial;
        probe.reference_run(expected, initial.pc + 8);
        auto before = nce_direct_stats(engine);
        uint64_t fault = 0;
        auto exit = run(actual, fault);
        if (exit != NCE_DIRECT_UNSUPPORTED || std::memcmp(&actual, &expected, sizeof(actual))) {
            std::fprintf(stderr, "cross-page word=%08x bits=%u result=%u pc=%llx/%llx\n", word, bits, exit, actual.pc, expected.pc);
            for (unsigned i = 0; i < 31; ++i)
                if (actual.x[i] != expected.x[i])
                    std::fprintf(stderr, "x%u %llx/%llx\n", i, actual.x[i], expected.x[i]);
            CHECK(false);
        }
        for (unsigned page = 0; page < 4; ++page)
            CHECK(!std::memcmp(reinterpret_cast<unsigned char *>(probe.reference.data()) + page * page_bytes, physical(page), page_bytes));
        auto after = nce_direct_stats(engine);
        CHECK(after.cross_page_completed == before.cross_page_completed + 1);
        CHECK(after.cross_page_faults == before.cross_page_faults && after.cross_page_fallbacks == before.cross_page_fallbacks);
    }
};
NceDirectState state(std::mt19937_64 &random, unsigned rn, uint64_t base)
{
    NceDirectState value{};
    for (auto &reg : value.x)
        reg = random();
    for (auto &vector : value.v) {
        vector[0] = random();
        vector[1] = random();
    }
    if (rn != 31)
        value.x[rn] = base;
    value.sp = base;
    value.pc = CodeVA;
    value.nzcv = random() & 0xf0000000;
    value.fpsr = random() & 0x0800009f;
    value.access_flags = NCE_DIRECT_UNALIGNED;
    value.exclusive_address = UINT64_MAX;
    return value;
}
void check_accesses(Crossing &test, std::mt19937_64 &random)
{
    constexpr unsigned bases[] = {0, 16, 17, 18, 26, 27, 28, 29, 30};
    for (bool vector : {false, true}) {
        for (unsigned shift = 1; shift <= (vector ? 4U : 3U); ++shift) {
            for (unsigned opc = 0; opc < (vector ? 2U : shift == 3 ? 2U
                                                    : shift == 2   ? 3U
                                                                   : 4U);
                 ++opc) {
                unsigned encoded_size = shift == 4 ? 0 : shift;
                unsigned encoded_op = opc + (vector && shift == 4 ? 2 : 0);
                for (unsigned rn : bases) {
                    for (unsigned first = 1; first < (1U << shift); ++first) {
                        uint64_t address = DataVA + test.page_bytes - first;
                        for (unsigned mode = 0; mode < 5; ++mode) {
                            unsigned rd = (rn + 13) & 31;
                            uint32_t word = (encoded_size << 30) | (vector << 26) | (encoded_op << 22) | (rn << 5) | rd;
                            int64_t offset = mode == 0 ? 3 << shift : mode == 1 ? -7
                                                                  : mode == 2   ? 13
                                                                  : mode == 3   ? -9
                                                                                : -4;
                            auto initial = state(random, rn, mode == 3 ? address : address - offset);
                            if (mode == 0)
                                word |= 0x39000000 | (3 << 10);
                            else if (mode == 4) {
                                unsigned rm = rn == 17 ? 26 : 17;
                                word |= 0x38200800 | (6 << 13) | (rm << 16);
                                initial.x[rm] = uint32_t(-4);
                            } else
                                word |= 0x38000000 | ((uint32_t(offset) & 511) << 12) | (mode == 2 ? 3 << 10 : mode == 3 ? 1 << 10
                                                                                                                         : 0);
                            test.compare(word, initial);
                        }
                    }
                }
            }
        }
    }
    for (bool vector : {false, true}) {
        for (unsigned shift = 2; shift <= (vector ? 4U : 3U); ++shift) {
            for (unsigned load = 0; load < 2; ++load) {
                for (unsigned mode = 0; mode < 4; ++mode) {
                    for (unsigned rn : {0U, 16U, 17U, 18U, 26U, 27U, 28U, 29U, 30U, 31U}) {
                        unsigned bytes = 2U << shift;
                        for (unsigned first = 1; first < bytes; ++first) {
                            int64_t offset = (mode & 1 ? -2 : 3) * int64_t(1U << shift);
                            uint64_t address = DataVA + test.page_bytes - first;
                            uint64_t base = mode == 1 ? address : address - offset;
                            if (rn == 31 && (base & 15))
                                continue;
                            unsigned rd = (rn + 7) & 31, rt2 = (rd + 1) & 31;
                            uint32_t word = 0x28000000 | ((vector ? shift - 2 : 2 * (shift - 2)) << 30) |
                                            (vector << 26) | (load << 22) | (mode << 23) |
                                            ((uint32_t(offset >> shift) & 127) << 15) | (rt2 << 10) | (rn << 5) | rd;
                            test.compare(word, state(random, rn, base));
                        }
                    }
                }
            }
        }
    }
}
void check_structures(Crossing &test, std::mt19937_64 &random)
{
    for (unsigned q = 0; q < 2; ++q) {
        for (unsigned size = 0; size < 4; ++size) {
            for (unsigned load = 0; load < 2; ++load) {
                for (unsigned opcode : {0U, 2U, 4U, 6U, 7U, 8U, 10U}) {
                    uint32_t word = 0x0c000000 | (q << 30) | (load << 22) | (opcode << 12) | (size << 10) | (18 << 5) | 31;
                    if (nce_direct::decode(word) != nce_direct::Structure)
                        continue;
                    auto access = nce_direct::memory_access(word, nce_direct::Structure);
                    for (unsigned mode = 0; mode < 3; ++mode) {
                        uint32_t op = word | (mode ? (1 << 23) | ((mode == 1 ? 31U : 18U) << 16) : 0);
                        for (unsigned first : {1U, access.bytes / 2, access.bytes - 1})
                            test.compare(op, state(random, 18, DataVA + test.page_bytes - first));
                    }
                }
                for (bool replicate : {false, true}) {
                    if (replicate && !load)
                        continue;
                    for (unsigned count = 1; count <= 4; ++count) {
                        unsigned scale = replicate ? 3 : size == 3 ? 2
                                                                   : size;
                        unsigned encoded_size = replicate ? size : size == 0 ? 3
                                                               : size == 1   ? 2
                                                               : size == 3   ? 1
                                                                             : 0;
                        uint32_t word = 0x0d000000 | (q << 30) | (load << 22) | (((count - 1) & 1) << 21) |
                                        (((count - 1) >> 1) << 13) | (scale << 14) | (!replicate && size < 3 ? 1 << 12 : 0) |
                                        (encoded_size << 10) | (17 << 5) | 31;
                        auto access = nce_direct::memory_access(word, nce_direct::StructureLane);
                        if (access.bytes < 2)
                            continue;
                        for (unsigned mode = 0; mode < 3; ++mode) {
                            uint32_t op = word | (mode ? (1 << 23) | ((mode == 1 ? 31U : 17U) << 16) : 0);
                            for (unsigned first : {1U, access.bytes / 2, access.bytes - 1})
                                test.compare(op, state(random, 17, DataVA + test.page_bytes - first));
                        }
                    }
                }
            }
        }
    }
}
void check_faults(Crossing &test, std::mt19937_64 &random)
{
    for (bool write : {false, true}) {
        uint32_t word = write ? 0xa8810801 : 0xa8c10801;
        test.load(word);
        auto initial = state(random, 0, DataVA + test.page_bytes - 8), actual = initial;
        auto before = test.ram;
        (write ? test.writable : test.readable)[1] = false;
        ++test.epoch;
        uint64_t fault = 0;
        CHECK(test.run(actual, fault) == (write ? NCE_DIRECT_WRITE_FAULT : NCE_DIRECT_READ_FAULT));
        CHECK(fault == DataVA + test.page_bytes && !std::memcmp(&actual, &initial, sizeof(actual)) && before == test.ram);
        (write ? test.writable : test.readable)[1] = true;
        ++test.epoch;
        test.compare(word, initial);
    }
    test.load(0xa8810801);
    test.alias_code = true;
    ++test.epoch;
    auto initial = state(random, 0, DataVA + test.page_bytes - 8), actual = initial;
    auto before = test.ram;
    auto code = test.probe.code;
    uint64_t fault = 0;
    CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT);
    CHECK(fault == DataVA + test.page_bytes && !std::memcmp(&actual, &initial, sizeof(actual)) && before == test.ram && code == test.probe.code);
    test.alias_code = false;
    ++test.epoch;
    initial.pc = CodeVA + 4092;
    test.compare(0xa8c10801, initial);
    for (unsigned format = 0; format < 6; ++format) {
        if (format % 3 == 0 || (!(format >= 3) && format % 3 == 2))
            continue;
        uint64_t address = DataVA + test.page_bytes - 4;
        uint32_t word = 0x18000000 | ((format % 3) << 30) | (format >= 3 ? 1U << 26 : 0) |
                        (uint32_t((address - CodeVA) / 4) << 5) | 18;
        test.compare(word, state(random, 0, 0));
    }
    auto stats = nce_direct_stats(test.engine);
    CHECK(stats.cross_page_exits == stats.cross_page_completed + stats.cross_page_faults + stats.cross_page_fallbacks);
    CHECK(stats.cross_page_faults == 3 && !stats.cross_page_fallbacks);
}
}  // namespace

void direct_cross_page_tests(Probe &probe)
{
    std::mt19937_64 random(0x43524f5353);
    for (unsigned bits : {12U, 14U}) {
        auto holder = std::make_unique<Crossing>(probe, bits);
        check_accesses(*holder, random);
        check_structures(*holder, random);
        check_faults(*holder, random);
        std::printf("{\"test\":\"cross_page_noncontiguous_memory_vectors_faults_%u\",\"passed\":true}\n", bits);
    }
}
