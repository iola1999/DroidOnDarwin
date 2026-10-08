/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)

struct Counter {
    uint64_t value = 10;
    bool allowed = true;
    static HuskArm64Assist Read(void *opaque, HuskArm64State *state, uint32_t instruction) {
        auto &self = *static_cast<Counter *>(opaque);
        if (!self.allowed || (instruction & 0xffffffe0) != 0xd53be040)
            return HUSK_ASSIST_DECLINED;
        if ((instruction & 31) != 31) state->x[instruction & 31] = self.value;
        ++self.value;
        state->pc += 4;
        return HUSK_ASSIST_CONTINUE;
    }
};

int main()
{
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(
        husk_direct_create(), husk_direct_destroy);
    std::array<uint32_t, 1024> code;
    code.fill(0xd4000001);
    HuskArm64KernelReads values{0x123456789abcdef0, 0xffff800012340000, 0x3c0};
    HuskArm64Memory memory{};
    memory.kernel_reads = &values;
    for (auto [instruction, value] : std::array<std::pair<uint32_t, uint64_t>, 3>{{
            {0xd538d080, values.tpidr_el1}, {0xd5384100, values.sp_el0}, {0xd53b4220, values.daif}}}) {
        for (unsigned reg = 0; reg < 32; ++reg) {
            code[0] = instruction | reg;
            HuskArm64State state{};
            for (unsigned i = 0; i < 31; ++i) state.x[i] = 0x9876543210 + i;
            state.pc = 0x10000; state.nzcv = 0xb0000000;
            auto expected = state;
            expected.pc += 4;
            if (reg != 31) expected.x[reg] = value;
            CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
            CHECK(!std::memcmp(&state, &expected, sizeof(state)));
            // Reusing the same instruction without permission must stop at MRS.
            memory.kernel_reads = nullptr;
            state.pc = 0x10000; expected = state;
            CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
            CHECK(!std::memcmp(&state, &expected, sizeof(state)));
            memory.kernel_reads = &values;
        }
    }
    code[0] = 0xd5384100;
    for (unsigned i = 0; i < 8; ++i) {
        ++values.sp_el0;
        HuskArm64State state{}; state.pc = 0x10000;
        CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
        CHECK(state.x[0] == values.sp_el0 && state.pc == 0x10004);
    }
    memory.kernel_reads = nullptr;
    Counter counter;
    memory.opaque = &counter; memory.assist = Counter::Read;
    code[0] = 0xd53be040; code[1] = 0xd53be041; code[2] = 0xd53be05f;
    code[3] = 0xcb000022; // sub x2,x1,x0
    HuskArm64State state{}; state.pc = 0x10000;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x10010 && state.x[0] == 10 && state.x[1] == 11 && state.x[2] == 1 && counter.value == 13);
    counter.allowed = false; state.pc = 0x10000;
    auto expected = state;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
    CHECK(!std::memcmp(&state, &expected, sizeof(state)));
    std::puts("kernel register reads, changing values, denied access and counter continuation PASS");
}
