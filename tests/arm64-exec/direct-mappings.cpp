/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sys/mman.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct Memory {
    uint8_t *ram = static_cast<uint8_t *>(mmap(nullptr, 32768, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANON, -1, 0));
    unsigned bank = 0, probes = 0;
    bool readable = true, writable = true;
    Memory() { CHECK(ram != MAP_FAILED); }
    ~Memory() { munmap(ram, 32768); }
    static void *Map(void *opaque, uint64_t page, bool write) {
        auto &m = *static_cast<Memory *>(opaque);
        ++m.probes;
        if (page < 0x20000 || page >= 0x24000 || !(write ? m.writable : m.readable)) return nullptr;
        return m.ram + m.bank + page - 0x20000;
    }
    static void *OtherMap(void *opaque, uint64_t page, bool write) {
        auto *mapped = static_cast<uint8_t *>(Map(opaque, page, write));
        return mapped ? mapped + 16384 : nullptr;
    }
    uint64_t &value(unsigned offset = 0) { return *reinterpret_cast<uint64_t *>(ram + offset); }
};

int main(int argc, char **argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--fastmem")) setenv("HUSK_ARM64_FASTMEM", "1", 1);
    setenv("HUSK_ARM64_LOOP_MAP", "0", 1);
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(husk_direct_create(), husk_direct_destroy);
    Memory a, b;
    a.value() = 11; a.value(16384) = 22; b.value() = 33;
    std::array<uint32_t, 1024> code;
    code.fill(0xd4000001); code[0] = 0xf9400020; // ldr x0,[x1]
    HuskArm64Memory memory{};
    memory.opaque = &a; memory.map_page = Memory::Map; memory.mapping_epoch = 1;
    auto run = [&](bool completes, uint64_t value) {
        HuskArm64State state{};
        state.pc = 0x10000; state.x[1] = 0x20000;
        auto expected = state;
        if (completes) { expected.pc += 4; expected.x[0] = value; }
        CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&state, &expected, sizeof(state)));
    };
    run(true, 11);
    unsigned probes = a.probes;
    for (unsigned i = 0; i < 100; ++i) {
        a.value() = i;
        run(true, i);
    }
    CHECK(a.probes == probes);
    memory.opaque = &b;
    run(true, 33); CHECK(b.probes > 0);
    memory.opaque = &a;
    run(true, 99);
    memory.map_page = Memory::OtherMap;
    run(true, 22);
    memory.map_page = Memory::Map;
    run(true, 99);
    a.bank = 16384; ++memory.mapping_epoch;
    run(true, 22);
    a.readable = false; ++memory.mapping_epoch;
    run(false, 0);
    a.readable = true; ++memory.mapping_epoch;
    run(true, 22);

    // A zero generation requires revalidation even when owner/callback match.
    memory.mapping_epoch = 0;
    a.bank = 0; run(true, 99);
    a.bank = 16384; run(true, 22);
    a.readable = false; run(false, 0);
    a.readable = true; a.bank = 0;

    // Every new entry may introduce a physical alias of previously writable
    // data. The actual code page must remain intact after write refusal.
    code[0] = 0xf9000020; // str x0,[x1]
    memory.mapping_epoch = 10;
    HuskArm64State state{};
    state.pc = 0x10000; state.x[0] = 77; state.x[1] = 0x20100;
    CHECK(husk_direct_run(engine.get(), &state, 0x10000, code.data(), memory, 100) == HUSK_ARM64_FALLBACK);
    CHECK(a.value(256) == 77);
    std::memcpy(a.ram, code.data(), 4096);
    a.writable = false;
    state = {}; state.pc = 0x30000; state.x[0] = 88; state.x[1] = 0x20000;
    auto expected = state;
    CHECK(husk_direct_run(engine.get(), &state, 0x30000, a.ram, memory, 100) == HUSK_ARM64_FALLBACK);
    CHECK(!std::memcmp(&state, &expected, sizeof(state)));
    CHECK(!std::memcmp(a.ram, code.data(), 4096));

    a.writable = true; a.value() = 7; code[0] = 0xf9400020;
    for (bool retain : {false, true}) {
        memory.mapping_epoch = retain ? 20 : 0;
        a.probes = 0;
        auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 100000; ++i) run(true, 7);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        std::printf("retain=%d slices=100000 probes=%u us=%lld\n", retain, a.probes, (long long)us);
        CHECK(retain ? a.probes <= 4 : a.probes >= 100000);
    }
    std::puts("mapping owners, callbacks, generations, permissions and code aliases PASS");
}
