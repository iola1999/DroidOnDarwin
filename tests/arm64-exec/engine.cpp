/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <pthread.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::abort(); } } while (0)

struct Memory {
    std::array<uint8_t, 4096> ram{};
    std::array<uint8_t, 4096> collision_ram{};
    bool writable = true;
    bool tagged = false;
    bool jit_write_on_map = false;
    unsigned reads = 0;
    unsigned writes = 0;
    uint8_t *Locate(uint64_t a, size_t n)
    {
        if (tagged) a &= UINT64_C(0x00ffffffffffffff);
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
        if (m.jit_write_on_map) pthread_jit_write_protect_np(0);
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
        // A later entry can branch into an already compiled region on the same
        // page. Its conditional target must bind to that region's actual code.
        program({0x91000400, 0xd4000001});
        code[32] = 0xb4000001 | ((unsigned(-32) & 0x7ffff) << 5); // cbz x1,-128
        code[33] = 0xd4000001;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 1);
        state.pc = 0x10080;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 2 && state.pc == 0x10004);
        code[0] = 0x91000800; // add x0,x0,2 after both entries were cached
        state.pc = 0x10080;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 4);

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
    CHECK(run() == (direct ? HUSK_ARM64_BRANCH : HUSK_ARM64_FALLBACK) && state.pc == 0x11000);
    for (uint32_t unsupported : {0xd4000001U, 0xc85f7c20U,
                                 0xc8027c20U,
                                 0x1ac24020U, 0xba000400U, 0x91800000U}) {
        CHECK(!husk_arm64_supported(unsupported));
        program({unsupported});
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000);
    }
    if (direct) {
        program({0xf9400020, 0xd4000001}); // ldr x0,[x1]; svc 0
        state.x[1] = 0x20000;
        memory.ram.fill(0);
        memory.ram[0] = 42;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 42);
        state.pc = 0x10000;
        memory.jit_write_on_map = true;
        pthread_jit_write_protect_np(0);
        CHECK(run() == HUSK_ARM64_FALLBACK && state.x[0] == 42);
        memory.jit_write_on_map = false;
        for (uint32_t fp : {0x4e208400U, 0x1e202800U, 0x3dc00020U,
                            0x3d800020U, 0xd53b4402U, 0xd53b4422U}) {
            program({0x91000400, fp, 0xd4000001});
            state.access_flags = 2;
            state.x[1] = 0x20000;
            state.v[0][0] = 0x12345678;
            auto expected = state;
            expected.pc += 4;
            expected.x[0] = 1;
            auto old_memory = memory.ram;
            CHECK(run() == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
            CHECK(memory.ram == old_memory);
            state.access_flags = 0;
            CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10008);
        }
        program({0xd503201f, 0xd65f0000}); // nop; ret x0 to a non-leader within this page
        state.x[0] = 0x10004;
        CHECK(run(20) == HUSK_ARM64_BUDGET && state.pc == 0x10004);
        program({0xf8408420, 0xd4000001}); // ldr x0,[x1],8
        state.x[0] = 17;
        state.x[1] = UINT64_C(0xb400000000020000);
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000 && state.x[0] == 17);
        memory.tagged = true;
        memory.ram.fill(0);
        memory.ram[0] = 91;
        state.pc = 0x10000;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10004 && state.x[0] == 91);
        CHECK(state.x[1] == UINT64_C(0xb400000000020008));
        program({0xf9400020, 0x91000400, 0xf9000020, 0xf1000442, 0x54ffff81, 0xd4000001});
        state.x[1] = UINT64_C(0xb400000000020000);
        state.x[2] = 10;
        CHECK(run() == HUSK_ARM64_FALLBACK && memory.ram[0] == 101);
        memory.tagged = false;
        state.pc = 0x10000;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000);

        program({0xad400440, 0xd4000001}); // ldp q0,q1,[x2]
        state.x[2] = 0x20003;
        for (unsigned i = 0; i < 32; ++i) memory.ram[i + 3] = i + 17;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000);
        state.access_flags = 1;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10004);
        CHECK(!std::memcmp(state.v, memory.ram.data() + 3, 32));
        state.pc = 0x10000;
        state.x[2] = 0x20fe1;
        auto unaligned_before = state;
        CHECK(run() == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &unaligned_before, sizeof(state)));
        state.x[2] = 0x20003;
        state.access_flags = 0;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10000);

        program({0xd53bd050, 0xd53b4212, 0xd53b4401, 0xd53b443f, 0xd4000001});
        state.tpidr_el0 = 0x123456789abcdef0;
        state.nzcv = 0xb0000000;
        state.fpcr = 0x03000000;
        CHECK(run() == HUSK_ARM64_FALLBACK && state.pc == 0x10010);
        CHECK(state.x[16] == state.tpidr_el0 && state.x[18] == state.nzcv && state.x[1] == state.fpcr);
        // Vector faults preserve both registers, NZCV, FPSR and writeback.
        program({0xacc10440, 0xd4000001}); // ldp q0,q1,[x2],32
        state.x[2] = 0x20ff0;
        state.nzcv = 0xa0000000;
        state.fpsr = 0x08000000;
        for (auto &v : state.v) { v[0] = 17; v[1] = 23; }
        auto before_state = state;
        CHECK(run() == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &before_state, sizeof(state)));
        program({0xad810440, 0xd4000001}); // stp q0,q1,[x2,32]!
        state.x[2] = 0x20000;
        memory.writable = false;
        auto before_memory = memory.ram;
        before_state = state;
        CHECK(run() == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &before_state, sizeof(state)));
        CHECK(memory.ram == before_memory);
        memory.writable = true;
        program({0x1e202800, 0xd4000001}); // fadd s0,s0,s0
        state.fpcr = 1U << 8; // Guest exception traps stay with QEMU.
        before_state = state;
        CHECK(run() == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &before_state, sizeof(state)));

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
