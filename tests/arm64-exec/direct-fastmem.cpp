/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <signal.h>
#include <sys/mman.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct Memory {
    static constexpr size_t bytes = 65536;
    uint8_t *ram = static_cast<uint8_t *>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANON, -1, 0));
    uint64_t base = 0x20000;
    size_t bank = 0;
    std::array<bool, 4> readable{true, true, true, true}, writable = readable;
    std::array<unsigned, 4> offsets{0, 4096, 8192, 12288};
    bool code_protected = false;
    unsigned probes = 0;
    Memory() { CHECK(ram != MAP_FAILED); }
    ~Memory() { munmap(ram, bytes); }
    uint64_t &value() { return *reinterpret_cast<uint64_t *>(ram + bank); }
    static void *Map(void *opaque, uint64_t page, bool write) {
        auto &m = *static_cast<Memory *>(opaque);
        ++m.probes;
        page &= UINT64_C(0x00ffffffffffffff);
        if (page < m.base || page - m.base >= 16384) return nullptr;
        unsigned index = (page - m.base) / 4096;
        if (!(write ? m.writable : m.readable)[index]) return nullptr;
        if (write && m.code_protected && !m.offsets[index]) return nullptr;
        return m.ram + m.bank + m.offsets[index];
    }
    static const void *Code(void *opaque, uint64_t page) {
        auto &m = *static_cast<Memory *>(opaque);
        if (page != 0x30000) return nullptr;
        m.code_protected = true;
        return m.ram + m.bank;
    }
};

int main()
{
    setenv("HUSK_ARM64_FASTMEM", "1", 1);
    setenv("HUSK_ARM64_LOOP_MAP", "0", 1);
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGBUS);
    CHECK(!pthread_sigmask(SIG_BLOCK, &blocked, nullptr));
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(
        husk_direct_create(), husk_direct_destroy);
    Memory m;
    std::array<uint32_t, 1024> code;
    uint64_t epoch = 0;
    code.fill(0xd4000001);
    auto initial = [&] {
        HuskArm64State state{};
        for (unsigned r = 0; r < 31; ++r) state.x[r] = 0x12345000 + r * 17;
        for (unsigned r = 0; r < 32; ++r) {
            state.v[r][0] = 0xabc000 + r;
            state.v[r][1] = 0xdef000 + r;
        }
        state.pc = 0x10000; state.x[1] = m.base; state.sp = m.base + 0x100;
        state.nzcv = 0xb0000000; state.fpsr = 0x08000091; state.fpcr = 0x01000000;
        return state;
    };
    auto run = [&](HuskArm64State &state) {
        return husk_direct_run(engine.get(), &state, 0x10000, code.data(),
                              {&m, nullptr, nullptr, Memory::Map, nullptr, Memory::Code, epoch}, 65536);
    };
    code[0] = 0xf9400020; code[1] = 0x91000400; code[2] = 0xf9000020;
    m.value() = 41;
    auto state = initial(), expected = state;
    expected.x[0] = 42; expected.pc += 12;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    CHECK(m.value() == 42 && m.probes == 8);
    std::puts("PASS mapped read/write faults preserve complete guest state");

    epoch = 1;
    state = initial();
    CHECK(run(state) == HUSK_ARM64_FALLBACK && m.value() == 43);
    unsigned probes = m.probes;
    for (unsigned i = 0; i < 64; ++i) {
        state = initial();
        CHECK(run(state) == HUSK_ARM64_FALLBACK && m.value() == 44 + i);
    }
    CHECK(m.probes == probes);
    m.bank = 16384; m.value() = 99; ++epoch;
    state = initial();
    CHECK(run(state) == HUSK_ARM64_FALLBACK && m.value() == 100 && m.probes == probes + 8);
    m.writable.fill(false); ++epoch;
    state = initial();
    CHECK(run(state) == HUSK_ARM64_FALLBACK && state.pc == 0x10008 && m.value() == 100);
    m.writable.fill(true);
    epoch = 0; m.bank = 0;
    std::puts("PASS retained mappings, generation replacement and permission revocation");

    for (unsigned i = 0; i < 64; ++i) {
        m.bank = (i & 1) * 16384;
        m.base = (i & 2) ? UINT64_C(0x7fff00020000) : 0x20000;
        m.value() = i;
        state = initial();
        CHECK(run(state) == HUSK_ARM64_FALLBACK && m.value() == i + 1 && state.x[0] == i + 1);
    }
    m.base = 0x20000; m.bank = 0;
    std::puts("PASS address-space replacement and sparse high virtual addresses");

    // Reusing compiled code after a permission change must fault before storing.
    code.fill(0xd4000001); code[0] = 0xf9000020;
    state = initial();
    CHECK(run(state) == HUSK_ARM64_FALLBACK && state.pc == 0x10004);
    uint64_t before = m.value();
    m.writable.fill(false);
    state = initial(); state.x[0] = 99; expected = state;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    CHECK(m.value() == before);
    m.writable.fill(true);
    std::puts("PASS revoked write permission");

    code[0] = 0xf9400020;
    m.readable[1] = false;
    state = initial();
    CHECK(run(state) == HUSK_ARM64_FALLBACK && state.pc == 0x10004 && state.x[0] == before);
    state = initial(); state.x[1] += 4096; expected = state;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    m.readable[1] = true;
    std::puts("PASS mixed 4 KiB permissions use checked access");

    m.offsets = {12288, 8192, 4096, 0};
    *reinterpret_cast<uint64_t *>(m.ram + 12288) = 73;
    code[0] = 0xf9400031; // ldr x17,[x1]
    state = initial(); expected = state; expected.x[17] = 73; expected.pc += 4;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    m.offsets = {0, 4096, 8192, 12288};
    std::puts("PASS physically fragmented group uses checked access");

    code[0] = 0xf8408420; // ldr x0,[x1],#8
    state = initial(); state.x[1] |= UINT64_C(0xb400000000000000);
    expected = state; expected.x[0] = m.value(); expected.x[1] += 8; expected.pc += 4;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    std::puts("PASS tagged address and architectural writeback");

    state = initial(); state.access_flags = 8;
    state.x[1] |= UINT64_C(0xb400000000000000);
    expected = state; expected.x[0] = m.value(); expected.x[1] += 8; expected.pc += 4;
    probes = m.probes;
    epoch = 90;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    CHECK(m.probes == probes + 4);
    state = initial(); state.access_flags = 8;
    state.x[1] |= UINT64_C(0xe300000000000000);
    expected = state; expected.x[0] = m.value(); expected.x[1] += 8; expected.pc += 4;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    CHECK(m.probes == probes + 4);
    epoch = 0;
    std::puts("PASS data TBI preserves tags and reuses the validated RAM alias");

    code[0] = 0xf9000020;
    state = initial(); state.access_flags = 1; state.x[1] += 4092; expected = state;
    std::array<uint8_t, 16> boundary;
    std::memcpy(boundary.data(), m.ram + 4088, boundary.size());
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    CHECK(!std::memcmp(boundary.data(), m.ram + 4088, boundary.size()));
    std::puts("PASS cross-page store has no partial write");

    code[0] = 0x3dc00020; // ldr q0,[x1]
    state = initial(); expected = state;
    std::memcpy(expected.v[0], m.ram, 16); expected.pc += 4;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && !std::memcmp(&state, &expected, sizeof(state)));
    std::puts("PASS SIMD state across native memory faults");

    code.fill(0xd4000001);
    code[0] = 0x52800020; code[1] = 0xb9000020; code[2] = 0x94007ffe;
    auto *callee = reinterpret_cast<uint32_t *>(m.ram);
    callee[0] = 0x52800040; callee[1] = 0xb9000020; callee[2] = 0xd4000001;
    callee[192] = 7;
    state = initial(); state.x[1] += 0x300;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && state.pc == 0x30004);
    CHECK(callee[192] == 1 && state.x[0] == 2);
    std::puts("PASS new code page revokes mapped physical write aliases");

    code[2] = 0xd4000001;
    epoch = 100; m.code_protected = false;
    state = initial(); state.x[1] += 0x300;
    CHECK(run(state) == HUSK_ARM64_FALLBACK && state.pc == 0x10008);
    m.code_protected = true;
    state = initial(); state.pc = 0x30000; state.x[1] += 0x300;
    CHECK(husk_direct_run(engine.get(), &state, 0x30000, m.ram,
              {&m, nullptr, nullptr, Memory::Map, nullptr, Memory::Code, epoch}, 65536) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x30004 && callee[192] == 1 && state.x[0] == 2);
    std::puts("PASS retained write alias is revoked on next code entry");
    std::puts("ARM64-FASTMEM PASS");
}
