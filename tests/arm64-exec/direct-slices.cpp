/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)

struct Memory {
    std::array<uint32_t, 1024> a{}, b{};
    bool b_executable = true;
    bool protected_b = false;
    unsigned code_probes = 0;
    bool chain_assist = false;
    bool pending_landing = false, data_allowed = true;
    alignas(16384) std::array<uint8_t, 16384> data{};
    unsigned assists = 0;
    uint64_t base = 0x10000;

    static const void *Code(void *opaque, uint64_t page) {
        auto &m = *static_cast<Memory *>(opaque);
        ++m.code_probes;
        if (page == m.base) return m.a.data();
        if (page != m.base + 4096 || !m.b_executable) return nullptr;
        m.protected_b = true;
        return m.b.data();
    }
    static void *Data(void *opaque, uint64_t page, bool write) {
        auto &m = *static_cast<Memory *>(opaque);
        if (page >= 0x30000 && page < 0x34000)
            return m.data_allowed ? m.data.data() + page - 0x30000 : nullptr;
        // 0x21000 is a writable physical alias of code page B.
        if (page != 0x11000 && page != 0x21000) return nullptr;
        return write && m.protected_b ? nullptr : m.b.data();
    }
    static HuskArm64Assist Assist(void *opaque, HuskArm64State *state, uint32_t word) {
        auto &m = *static_cast<Memory *>(opaque);
        if ((word & 0xfffffc1f) != 0xd63f0000) return HUSK_ASSIST_DECLINED;
        ++m.assists;
        auto target = state->x[(word >> 5) & 31];
        state->x[30] = state->pc + 4;
        state->pc = target;
        if (m.pending_landing) {
            state->access_flags = (state->access_flags & ~48U) | 32;
            return HUSK_ASSIST_CHAIN_LANDING_CALL;
        }
        return m.chain_assist ? HUSK_ASSIST_CHAIN_CALL : HUSK_ASSIST_BRANCH;
    }
};

int main()
{
    Memory m;
    m.a.fill(0xd4000001); m.b.fill(0xd4000001);
    const uint32_t caller[] = {
        0xd290d402, 0xf2a00022, // mov x2,100000
        0xd2800000, 0x940003fd, // mov x0,0; bl B
        0xf1000442, 0x54ffffc1, 0xd4000001,
    };
    std::memcpy(m.a.data(), caller, sizeof(caller));
    m.b[0] = 0x91000400; m.b[1] = 0xd65f03c0;
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(
        husk_direct_create(), husk_direct_destroy);
    auto run = [&](bool chain) {
        HuskArm64State state{};
        state.pc = m.base;
        state.x[3] = m.base + 4096;
        auto start = std::chrono::steady_clock::now();
        unsigned slices = 0;
        HuskArm64Exit result;
        do {
            CHECK(++slices < 1000000);
            uint64_t page = state.pc & ~UINT64_C(4095);
            CHECK(page == m.base || page == m.base + 4096);
            m.protected_b = page == m.base + 4096;
            result = husk_direct_run(engine.get(), &state, page,
                page == m.base ? m.a.data() : m.b.data(),
                {&m, nullptr, nullptr, Memory::Data, Memory::Assist, chain ? Memory::Code : nullptr}, 4096);
        } while (result == HUSK_ARM64_BRANCH || result == HUSK_ARM64_BUDGET);
        CHECK(result == HUSK_ARM64_FALLBACK && state.pc == m.base + 24);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("chain=%d assist_chain=%d ns=%lld slices=%u result=%llu\n", chain, m.chain_assist,
                    (long long)ns, slices, (unsigned long long)state.x[0]);
        return state.x[0];
    };
    CHECK(run(false) == 100000);
    m.code_probes = 0;
    CHECK(run(true) == 100000);
    CHECK(m.code_probes < 1000);
    m.b[0] = 0x91000800;
    CHECK(run(true) == 200000);

    m.base = UINT64_C(0xffff800080010000);
    CHECK(run(true) == 200000);
    m.base = 0x10000;

    // Function-pointer calls must preserve the return address and reuse code
    // routes, while still ending a slice when its instruction budget expires.
    std::memcpy(m.a.data(), caller, sizeof(caller));
    m.a[3] = 0xd63f0060; // blr x3
    m.b[0] = 0x91000400;
    CHECK(run(true) == 100000);
    m.chain_assist = true;
    m.code_probes = 0;
    m.assists = 0;
    CHECK(run(true) == 100000);
    CHECK(m.assists > 0 && m.assists < 1000 && m.code_probes < 1000);
    m.b[0] = 0x91000800;
    CHECK(run(true) == 200000);

    m.pending_landing = true;
    m.assists = 0;
    CHECK(run(true) == 200000);
    CHECK(m.assists > 0 && m.assists < 1000);
    m.pending_landing = false;

    // Fetching a destination with no execute permission must end the slice.
    m.a.fill(0xd4000001); m.a[0] = 0x14000400;
    m.b_executable = false;
    HuskArm64State state{};
    state.pc = 0x10000;
    auto callbacks = HuskArm64Memory{&m, nullptr, nullptr, Memory::Data, nullptr, Memory::Code};
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), callbacks, 4096) == HUSK_ARM64_BRANCH);
    CHECK(state.pc == 0x11000 && !state.x[0]);
    m.a[0] = 0xd63f0060;
    state = {}; state.pc = 0x10000; state.x[3] = 0x11000;
    auto denied_call = HuskArm64Memory{&m, nullptr, nullptr, Memory::Data,
                                      Memory::Assist, Memory::Code};
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), denied_call, 4096) == HUSK_ARM64_BRANCH);
    CHECK(state.pc == 0x11000 && state.x[30] == 0x10004 && !state.x[0]);

    // A data mapping established before B becomes executable must lose write
    // access, including through a different virtual address of the same bytes.
    m.b_executable = true; m.protected_b = false;
    m.a[0] = 0x52800020; m.a[1] = 0xb9000020; m.a[2] = 0x940003fe;
    m.b[0] = 0x52800040; m.b[1] = 0xb9000020; m.b[2] = 0xd65f03c0;
    m.b[192] = 7;
    state = {}; state.pc = 0x10000; state.x[1] = 0x21300;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), callbacks, 4096) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x11004 && state.x[0] == 2 && m.b[192] == 1);

    m.a.fill(0xd4000001);
    m.a[0] = 0xd503201f; m.a[1] = 0xd63f0060; // nop; blr x3
    state = {}; state.pc = 0x10000; state.x[3] = 0x10004;
    m.assists = 0;
    callbacks.assist = Memory::Assist;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), callbacks, 16) == HUSK_ARM64_BUDGET);
    CHECK(state.pc == 0x10004 && m.assists > 0 && m.assists <= 17);

    // A failed first load retains the branch landing state. A completed load
    // clears it before the next instruction, including at a page boundary.
    m.a.fill(0xd4000001);
    m.a[0] = 0xf8408420; // ldr x0,[x1],#8
    uint64_t value = 0x123456789abcdef0;
    std::memcpy(m.data.data(), &value, 8);
    state = {}; state.pc = 0x10000; state.x[1] = 0x30000; state.access_flags = 17;
    auto initial = state;
    m.data_allowed = false;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), callbacks, 4096) == HUSK_ARM64_FALLBACK);
    CHECK(!std::memcmp(&initial, &state, sizeof(state)));
    m.data_allowed = true;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), callbacks, 4096) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x10004 && state.x[0] == value && state.x[1] == 0x30008 && state.access_flags == 1);
    m.a[1023] = m.a[0];
    state = initial; state.pc = 0x10ffc;
    m.b_executable = false;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), callbacks, 4096) == HUSK_ARM64_BRANCH);
    CHECK(state.pc == 0x11000 && state.x[0] == value && state.access_flags == 1);
    m.a.fill(0xd4000001); m.b.fill(0xd4000001);
    m.a[0] = 0xd63f0060; m.a[1] = 0xd2a00081; m.a[2] = 0xd63f0060;
    m.b[0] = 0xf9400020; m.b[1] = 0xd65f03c0;
    state = {}; state.pc = 0x10000; state.x[1] = 0x30000; state.x[3] = 0x11000;
    m.b_executable = true; m.pending_landing = true; m.assists = 0;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, m.a.data(), callbacks, 4096) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x11000 && state.x[0] == value && state.x[1] == 0x40000);
    CHECK(state.x[30] == 0x1000c && (state.access_flags & 48) == 32 && m.assists == 1);
    CHECK(husk_arm64_landing_supported(0xa9bf7bfd)); // stp x29,x30,[sp,#-16]!
    CHECK(!husk_arm64_landing_supported(0xd63f0060)); // blr x3
    CHECK(!husk_arm64_landing_supported(0xd503233f)); // paciasp
    std::puts("cross-page execution, invalidation and physical alias checks PASS");
}
