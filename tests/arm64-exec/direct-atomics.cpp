/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <sys/sysctl.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)

struct Memory {
    alignas(16384) std::array<uint8_t, 16384> bytes{};
    bool writable = true;
    bool readable = true, tbi = false;
    static void *Map(void *opaque, uint64_t page, bool write) {
        auto &m = *static_cast<Memory *>(opaque);
        if (m.tbi) page &= UINT64_C(0x00ffffffffffffff);
        return page >= 0x20000 && page < 0x24000 && (write ? m.writable : m.readable) ?
            m.bytes.data() + page - 0x20000 : nullptr;
    }
};

int main()
{
    int available = 0;
    size_t size = sizeof(available);
    if (sysctlbyname("hw.optional.arm.FEAT_LSE", &available, &size, nullptr, 0) || !available) {
        std::puts("host FEAT_LSE unavailable; skipped");
        return 0;
    }
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(
        husk_direct_create(), husk_direct_destroy);
    std::array<uint32_t, 1024> code{};
    code.fill(0xd4000001);
    std::mt19937_64 random(0x41544f4d4943);
    Memory memory;
    HuskArm64Memory callbacks{&memory, nullptr, nullptr, Memory::Map};
    unsigned samples = 0;
    for (unsigned width = 0; width < 4; ++width) {
        unsigned bits = 8U << width;
        uint64_t mask = UINT64_MAX >> (64 - bits);
        auto signed_value = [bits](uint64_t n) { return int64_t(n << (64 - bits)) >> (64 - bits); };
        for (unsigned operation = 0; operation < 10; ++operation) {
            for (unsigned sample = 0; sample < 256; ++sample) {
                HuskArm64State state{};
                for (auto &r : state.x) r = random();
                for (auto &v : state.v) { v[0] = random(); v[1] = random(); }
                state.pc = 0x10000; state.sp = 0x20040;
                state.access_flags = 4; state.nzcv = random() & 0xf0000000;
                unsigned rn = sample & 31, rs = random() & 31, rt = random() & 31;
                if (rn != 31) state.x[rn] = 0x20040;
                auto value = [&](unsigned reg) { return reg == 31 ? UINT64_C(0) : state.x[reg]; };
                uint64_t old = random() & mask, source = value(rs) & mask;
                // Exercise both compare-exchange outcomes, including Rs == Rn/Rt.
                if (operation == 9 && (sample & 1)) old = source;
                uint64_t expected = old;
                switch (operation) {
                case 0: expected = old + source; break;
                case 1: expected = old & ~source; break;
                case 2: expected = old ^ source; break;
                case 3: expected = old | source; break;
                case 4: expected = signed_value(old) > signed_value(source) ? old : source; break;
                case 5: expected = signed_value(old) < signed_value(source) ? old : source; break;
                case 6: expected = old > source ? old : source; break;
                case 7: expected = old < source ? old : source; break;
                case 8: expected = source; break;
                case 9: if (old == source) expected = value(rt); break;
                }
                expected &= mask;
                memory.bytes.fill(0xa5);
                std::memcpy(memory.bytes.data() + 64, &old, bits / 8);
                auto expected_memory = memory.bytes;
                std::memcpy(expected_memory.data() + 64, &expected, bits / 8);
                code[0] = (width << 30) | (rs << 16) | (rn << 5) | rt |
                    (operation == 9 ? 0x08a07c00 | ((sample & 1) << 22) | ((sample & 2) << 14) :
                                      0x38200000 | (operation << 12) | ((sample & 3) << 22));
                auto expected_state = state;
                expected_state.pc += 4;
                unsigned output = operation == 9 ? rs : rt;
                if (output != 31) expected_state.x[output] = old;
                auto result = husk_direct_run(engine.get(), &state, 0x10000, code.data(), callbacks, 1024);
                if (result != HUSK_ARM64_FALLBACK || std::memcmp(&state, &expected_state, sizeof(state)) ||
                    memory.bytes != expected_memory) {
                    std::fprintf(stderr, "atomic %08x sample=%u old=%llx source=%llx pc=%llx\n",
                        code[0], sample, (unsigned long long)old, (unsigned long long)source,
                        (unsigned long long)state.pc);
                    return 1;
                }
                ++samples;
            }
        }
    }
    // Feature gating, alignment and write permission must reject the operation
    // before changing either memory or architectural state.
    code[0] = 0xf8200022; // ldadd x0,x2,[x1]
    for (unsigned condition = 0; condition < 3; ++condition) {
        HuskArm64State state{};
        state.pc = 0x10000; state.x[0] = 123; state.x[1] = 0x20000;
        state.access_flags = condition == 0 ? 0 : 4;
        if (condition == 1) ++state.x[1];
        memory.writable = condition != 2;
        auto original = state;
        auto bytes = memory.bytes;
        CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), callbacks, 1024) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&state, &original, sizeof(state)) && memory.bytes == bytes);
    }
    std::printf("LSE values, register aliases and fault boundaries PASS (%u cases)\n", samples);
    memory.writable = true;
    for (unsigned width = 0; width < 4; ++width) for (unsigned sample = 0; sample < 256; ++sample) {
        unsigned bits = 8U << width;
        uint64_t mask = UINT64_MAX >> (64 - bits), old = random() & mask;
        unsigned base = sample & 31, value = random() & 31, status = random() & 31;
        memory.tbi = sample & 8;
        uint64_t address = 0x20040 | (memory.tbi ? UINT64_C(0xab00000000000000) : 0);
        HuskArm64State state{};
        for (auto &r : state.x) r = random();
        for (auto &v : state.v) { v[0] = random(); v[1] = random(); }
        state.pc = 0x10000; state.nzcv = random() & 0xf0000000;
        state.access_flags = memory.tbi ? 8 : 0;
        if (base == 31) state.sp = address; else state.x[base] = address;
        state.exclusive_address = state.exclusive_value = UINT64_MAX;
        std::memcpy(memory.bytes.data() + 64, &old, bits / 8);
        code[0] = (width << 30) | 0x085f7c00 | ((sample & 1) << 15) | (base << 5) | value;
        auto expected_state = state;
        expected_state.pc += 4; expected_state.exclusive_address = 0x20040;
        expected_state.exclusive_value = old;
        if (value != 31) expected_state.x[value] = old;
        auto bytes = memory.bytes;
        CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), callbacks, 1024) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&state, &expected_state, sizeof(state)) && memory.bytes == bytes);

        state.pc = 0x10000;
        if (base == 31) state.sp = address; else state.x[base] = address;
        // Preserve the software monitor across calls, as when TCG loaded it.
        if ((sample & 3) == 0) state.exclusive_value ^= 1;
        if ((sample & 3) == 1) state.exclusive_address += 4096;
        code[0] = (width << 30) | 0x08007c00 | ((sample & 2) << 14) |
            (status << 16) | (base << 5) | value;
        uint64_t replacement = value == 31 ? 0 : state.x[value];
        bool success = state.exclusive_address == 0x20040 && state.exclusive_value == old;
        expected_state = state; expected_state.pc += 4; expected_state.exclusive_address = UINT64_MAX;
        if (status != 31) expected_state.x[status] = !success;
        if (success) std::memcpy(bytes.data() + 64, &replacement, bits / 8);
        CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), callbacks, 1024) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&state, &expected_state, sizeof(state)) && memory.bytes == bytes);
    }
    memory.tbi = false;
    for (unsigned condition = 0; condition < 7; ++condition) {
        HuskArm64State state{};
        state.pc = 0x10000; state.x[0] = 0x123; state.x[1] = 0x20000;
        state.exclusive_address = 0x20000; state.exclusive_value = 0x123;
        code[0] = condition == 0 ? 0xc85ffc22 : 0xc803fc20; // ldaxr x2,[x1] / stlxr w3,x0,[x1]
        memory.readable = condition != 0; memory.writable = condition != 1;
        if (condition == 2) state.x[1] = state.exclusive_address = 0x20001;
        if (condition == 3) { state.access_flags = 8; state.x[1] = UINT64_C(0xffff000000020000); }
        if (condition == 4) { state.x[1] = 0; state.exclusive_address = UINT64_MAX; }
        if (condition == 5) code[0] = 0xd5033f5f; // clrex
        if (condition == 6) { code[0] = 0xc803ffe0; state.sp = 0x20001; }
        auto expected = state; auto bytes = memory.bytes;
        if (condition >= 4) {
            expected.pc += 4; expected.exclusive_address = UINT64_MAX;
            if (condition == 4 || condition == 6) expected.x[3] = 1;
        }
        CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), callbacks, 1024) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&state, &expected, sizeof(state)) && memory.bytes == bytes);
    }
    std::puts("exclusive load/store, monitor handoff, tags, CLREX and fault boundaries PASS");
}
