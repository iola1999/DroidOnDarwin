/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)

int main()
{
    // Expected signatures come from QEMU 10.0.12 qemu_xxhash64_4, seed 1.
    // The guest differential run also compares each native miss with QEMU.
    struct Vector { uint64_t pointer, mask; unsigned key; uint64_t signature; };
    const Vector vectors[] = {
        {0x0000000012345678ULL,0xffffff8000000000ULL,0,0x0275a88012345678ULL},
        {0x0000000012345678ULL,0xffffff8000000000ULL,1,0x964d688012345678ULL},
        {0xffff123456789abcULL,0xffff000000000000ULL,0,0x91be123456789abcULL},
        {0xffff123456789abcULL,0xffff000000000000ULL,1,0xe283123456789abcULL},
        {0xab00123456789abcULL,0x00ff000000000000ULL,0,0xab78123456789abcULL},
        {0xab00123456789abcULL,0x00ff000000000000ULL,1,0xab44123456789abcULL},
        {0xabff123456789abcULL,0x00ff000000000000ULL,0,0xabfb123456789abcULL},
        {0xabff123456789abcULL,0x00ff000000000000ULL,1,0xabd2123456789abcULL},
    };
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(
        husk_direct_create(), husk_direct_destroy);
    std::array<uint32_t, 1024> code;
    code.fill(0xd4000001);
    HuskArm64Pauth parameters{};
    HuskArm64Memory memory{};
    memory.pauth = &parameters;
    for (const auto &v : vectors) {
        for (unsigned i = 0; i < 4; ++i)
            parameters.keys[i] = {0x0011223344556677ULL + (i & 1),
                                  0x8899aabbccddeeffULL + (i & 1), v.mask, 1, 0};
        auto run = [&](uint32_t instruction, uint64_t input, uint64_t output, bool completes) {
            code[0] = instruction;
            HuskArm64State state{};
            for (unsigned i = 0; i < 31; ++i) state.x[i] = 0xabcdef0123456789ULL + i;
            for (unsigned i = 0; i < 32; ++i) {
                state.v[i][0] = 0x123456789abcdef0ULL + i;
                state.v[i][1] = 0xfedcba9876543210ULL - i;
            }
            state.pc = 0x10000;
            state.sp = 0x1234567890abcdefULL;
            state.nzcv = 0xb0000000;
            state.x[30] = input;
            auto expected = state;
            if (completes) { expected.pc += 4; expected.x[30] = output; }
            CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
            CHECK(!std::memcmp(&state, &expected, sizeof(state)));
        };
        run(v.key ? 0xd503237f : 0xd503233f, v.pointer, v.signature, true);
        run(v.key ? 0xd50323ff : 0xd50323bf, v.signature, v.pointer, true);
        run(0xd50320ff, v.signature, v.pointer, true);
        run(v.key ? 0xd50323ff : 0xd50323bf, v.signature ^ (1ULL << 50), 0, false);
        run(v.key ? 0xd503237f : 0xd503233f, v.pointer ^ (1ULL << 50), 0, false);
        for (auto &key : parameters.keys) key.enabled = 0;
        run(v.key ? 0xd503237f : 0xd503233f, v.pointer, v.pointer, true);
        run(v.key ? 0xd50323ff : 0xd50323bf, v.signature, v.signature, true);
    }
    std::puts("native PAC signatures, tags, keys, refusal and register preservation PASS");

    const uint32_t hints[] = {
        0xd50320ff, 0xd503211f, 0xd503215f, 0xd503219f, 0xd50321df,
        0xd503231f, 0xd503233f, 0xd503235f, 0xd503237f,
        0xd503239f, 0xd50323bf, 0xd50323df, 0xd50323ff,
    };
    unsigned assists = 0;
    memory = {};
    memory.opaque = &assists;
    memory.assist = [](void *opaque, HuskArm64State *, uint32_t word) {
        if (word != 0xd4000001) ++*static_cast<unsigned *>(opaque);
        return HUSK_ASSIST_DECLINED;
    };
    for (uint32_t hint : hints) {
        code[0] = hint;
        // Reuse the same code bytes while the owner's PAC controls change.
        for (bool inactive : {true, false, true}) {
            HuskArm64State state{};
            state.pc = 0x10000;
            state.sp = 0x1234567890abcdefULL;
            state.nzcv = 0xb0000000;
            state.access_flags = inactive ? HUSK_PAUTH_HINTS_NOP : 0;
            for (unsigned i = 0; i < 31; ++i) state.x[i] = 0xabc0123456789defULL + i;
            for (unsigned i = 0; i < 32; ++i) {
                state.v[i][0] = i + 11; state.v[i][1] = i + 19;
            }
            auto expected = state;
            if (inactive) expected.pc += 4;
            assists = 0;
            CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
            CHECK(!std::memcmp(&state, &expected, sizeof(state)));
            CHECK(assists == (inactive ? 0 : 1));
        }
    }
    // PACIA x0,x1 is outside the HINT space and must still leave Direct.
    code[0] = 0xdac10020;
    HuskArm64State state{};
    state.pc = 0x10000; state.access_flags = HUSK_PAUTH_HINTS_NOP;
    auto expected = state;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
    CHECK(!std::memcmp(&state, &expected, sizeof(state)));
    std::puts("inactive PAC hints, control transitions and explicit PAC fallback PASS");
}
