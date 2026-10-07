/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::abort(); } } while (0)

struct Memory {
    std::array<uint8_t, 4096> ram{};
    std::array<uint8_t, 4096> collision_ram{};
    bool writable = true;
    unsigned reads = 0;
    unsigned writes = 0;
    uint8_t *Locate(uint64_t a, size_t n)
    {
        uint64_t page = a & ~UINT64_C(4095);
        if (n > 4096 || (a & 4095) > 4096 - n) return nullptr;
        auto *base = page == 0x20000 ? ram.data() : page == 0x120000 ? collision_ram.data() : nullptr;
        return base ? base + (a & 4095) : nullptr;
    }
    static bool Read(void *opaque, uint64_t a, void *out, size_t n)
    {
        auto &m = *static_cast<Memory *>(opaque);
        m.reads++;
        auto *host = m.Locate(a, n);
        if (!host) return false;
        std::memcpy(out, host, n);
        return true;
    }
    static bool Write(void *opaque, uint64_t a, const void *in, size_t n)
    {
        auto &m = *static_cast<Memory *>(opaque);
        m.writes++;
        auto *host = m.Locate(a, n);
        if (!m.writable || !host) return false;
        std::memcpy(host, in, n);
        return true;
    }
    static void *Map(void *opaque, uint64_t page, bool write)
    {
        auto &m = *static_cast<Memory *>(opaque);
        return !write || m.writable ? m.Locate(page, 4096) : nullptr;
    }
};

int main()
{
    std::unique_ptr<HuskArm64Exec, decltype(&husk_arm64_destroy)> ctx(husk_arm64_create(), husk_arm64_destroy);
    CHECK(ctx);
    std::array<uint32_t, 1024> code{};
    Memory memory;
    HuskArm64Memory callbacks{&memory, Memory::Read, Memory::Write, Memory::Map};
    HuskArm64State state{};
    auto program = [&](std::initializer_list<uint32_t> words) {
        code.fill(0);
        std::copy(words.begin(), words.end(), code.begin());
        state = {};
        state.pc = 0x10000;
    };
    auto run = [&](uint64_t budget = 10000) {
        return husk_arm64_run(ctx.get(), &state, 0x10000, code.data(), callbacks, budget);
    };

    // mov x1,0; add x1,x1,x0; subs x0,x0,1; b.ne -8; svc 0
    program({0xd2800001, 0x8b000021, 0xf1000400, 0x54ffffc1, 0xd4000001});
    state.x[0] = 100;
    CHECK(run() == HUSK_ARM64_FALLBACK);
    CHECK(state.x[0] == 0 && state.x[1] == 5050);
    CHECK(state.pc == 0x10010 && state.nzcv == 0x60000000);

    // add x2,x2,1; ldr x0,[x1],8; add x2,x2,2; str x0,[x3]; svc 0
    const auto memory_program = [&] {
        program({0x91000442, 0xf8408420, 0x91000842, 0xf9000060, 0xd4000001});
        state.x[0] = 0xabcdef;
        state.x[1] = 0x20000;
        state.x[3] = 0x20010;
    };
    uint64_t value = 0x1234567890abcdef;
    std::memcpy(memory.ram.data(), &value, 8);
    memory_program();
    CHECK(run() == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x10010 && state.x[0] == value && state.x[1] == 0x20008 && state.x[2] == 3);
    CHECK(std::memcmp(memory.ram.data() + 16, &value, 8) == 0);

    memory_program();
    state.x[1] = 0x30000;
    CHECK(run() == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x10004 && state.x[0] == 0xabcdef);
    CHECK(state.x[1] == 0x30000 && state.x[2] == 1);

    program({0xf1000442, 0xf8408420, 0xb1000842, 0xd4000001});
    state.x[1] = 0x30000;
    state.x[2] = 1;
    CHECK(run() == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x10004 && state.x[2] == 0 && state.nzcv == 0x60000000);

    memory_program();
    memory.writable = false;
    memory.ram[16] = 0x77;
    CHECK(run() == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x1000c && state.x[0] == value && state.x[2] == 3);
    CHECK(memory.ram[16] == 0x77);
    memory.writable = true;

    program({0xf9400020, 0xf9400062, 0xf9400024, 0xd4000001});
    state.x[1] = 0x20000;
    state.x[3] = 0x120000;
    memory.ram.fill(0);
    memory.ram[0] = 17;
    memory.collision_ram[0] = 23;
    CHECK(run() == HUSK_ARM64_FALLBACK);
    CHECK(state.x[0] == 17 && state.x[2] == 23 && state.x[4] == 17);

    // A read crossing the mapped boundary must leave the destination untouched.
    memory_program();
    state.x[1] = 0x20ffc;
    CHECK(run() == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x10004 && state.x[0] == 0xabcdef && state.x[1] == 0x20ffc);

    // The next entry observes changed code and changed backing memory.
    program({0xd2800020, 0xd4000001}); // mov x0,1; svc 0
    CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 1);
    code[0] = 0xd2800040; // mov x0,2
    state.pc = 0x10000;
    CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 2);
    // Revisit pages after intervening code, including changes made while absent.
    std::array<uint32_t, 1024> second{};
    second[0] = 0xd2800060; // mov x0,3
    second[1] = 0xd4000001;
    for (unsigned i = 0; i < 2048; ++i) {
        state.pc = 0x11000 + uint64_t(i) * 4096;
        CHECK(husk_arm64_run(ctx.get(), &state, state.pc, second.data(), callbacks, 100) == HUSK_ARM64_FALLBACK);
        CHECK(state.x[0] == 3);
        code[0] = i & 1 ? 0xd2800040 : 0xd2800020;
        state.pc = 0x10000;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == (i & 1 ? 2 : 1));
    }
    Memory other;
    value = 99;
    std::memcpy(other.ram.data(), &value, 8);
    callbacks.opaque = &other;
    memory_program();
    CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 99);

    program({0x91000400, 0x17ffffff}); // add x0,x0,1; b -4
    CHECK(run(1000) == HUSK_ARM64_BUDGET);
    CHECK(state.x[0] >= 500 && state.x[0] <= 501);

    // Repeated accesses use validated RAM; a later entry must recheck permissions.
    callbacks.opaque = &memory;
    program({0xf9400020, 0x91000400, 0xf9000020, 0xf1000442, 0x54ffff81, 0xd4000001});
    memory.ram.fill(0);
    memory.reads = memory.writes = 0;
    state.x[1] = 0x20000;
    state.x[2] = 100;
    CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 100);
    bool direct = std::getenv("HUSK_ARM64_BACKEND") &&
                  !std::strcmp(std::getenv("HUSK_ARM64_BACKEND"), "direct");
    CHECK(memory.reads == (direct ? 0 : 1) && memory.writes == (direct ? 0 : 1));
    memory.writable = false;
    state.pc = 0x10000;
    CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10008);
    CHECK(memory.ram[0] == 100);
    memory.writable = true;

    if (direct) {
        // Keep the branch PC and link register intact for QEMU's BTYPE handling.
        for (uint32_t branch : {0xd61f0000U, 0xd63f0000U}) {
            program({0x91000421, branch});
            state.x[0] = 0x20000;
            state.x[30] = 0x1234;
            CHECK(run() == HUSK_ARM64_FALLBACK);
            CHECK(state.pc == 0x10004 && state.x[1] == 1 && state.x[30] == 0x1234);
        }
    }

    // A loop may use the registers reserved by the host for both data and base.
    program({0xf9000250, 0xf10007de, 0x54ffffc1, 0xd4000001});
    state.x[16] = 0x778899;
    state.x[18] = 0x20000;
    state.x[30] = 100;
    CHECK(run() == HUSK_ARM64_FALLBACK && state.x[30] == 0);
    std::memcpy(&value, memory.ram.data(), 8);
    CHECK(value == 0x778899 && state.x[16] == value && state.x[18] == 0x20000);

    // Changing the base on each iteration must retain per-access validation.
    program({0xf9400020, 0x91002021, 0xf1000442, 0x54ffffa1, 0xd4000001});
    memory.ram.fill(0);
    memory.ram[0] = 13;
    memory.ram[8] = 27;
    state.x[1] = 0x20000;
    state.x[2] = 2;
    CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 27 && state.x[1] == 0x20010);

    // An unchecked loop cannot span the end of a validated page.
    program({0xf9400020, 0xf9400423, 0xf1000442, 0x54ffffa1, 0xd4000001});
    state.x[1] = 0x20ff8;
    state.x[2] = 2;
    state.x[3] = 91;
    CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10004 && state.x[3] == 91);

    // A long guest TBZ must still branch when the generated page exceeds 32 KiB.
    code.fill(0xf9400020);
    code[0] = 0x36000000 | (1000U << 5); // tbz x0,0,+4000
    code[1000] = 0xd4000001;
    state = {};
    state.pc = 0x10000;
    CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10fa0);

    program({0x94000002, 0xd4000001, 0x91000400, 0xd65f03c0});
    CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10004);
    CHECK(state.x[0] == 1 && state.x[30] == 0x10004);

    program({0x14000400}); // b to the next code page
    CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x11000);
    for (uint32_t unsupported : {0xd4000001U, 0xd53bd040U, 0xc85f7c20U,
                                 0xc8027c20U, 0xad400440U, 0x4e228420U,
                                 0x1ac24020U, 0xba000400U, 0x91800000U}) {
        CHECK(!husk_arm64_supported(unsupported));
        program({unsupported});
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000);
    }
    if (direct) {
        // A pair crossing into an unmapped page must leave QEMU the whole access.
        program({0xa8c10440, 0xd4000001}); // ldp x0,x1,[x2],16
        state.x[0] = 17;
        state.x[1] = 23;
        state.x[2] = 0x20ff8;
        state.nzcv = 0xa0000000;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000);
        CHECK(state.x[0] == 17 && state.x[1] == 23 && state.x[2] == 0x20ff8 && state.nzcv == 0xa0000000);
        program({0xa9810440, 0xd4000001}); // stp x0,x1,[x2,16]!
        memory.writable = false;
        auto before = memory.ram;
        state.x[2] = 0x20000;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000 && state.x[2] == 0x20000);
        CHECK(memory.ram == before);
        memory.writable = true;
        for (uint32_t invalid : {0x9240fc00U, 0x12400000U, 0xf3400000U,
                                 0x13400000U, 0x13808000U, 0x93e00000U}) {
            CHECK(!husk_arm64_supported(invalid));
            program({invalid});
            CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000);
        }
    }
    std::puts("PASS: integer execution, flags, memory aborts, writeback, code invalidation, address-space switch, budget, fallback");
}
