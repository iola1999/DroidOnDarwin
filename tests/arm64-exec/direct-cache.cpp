/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using Page = std::array<uint32_t, 1024>;
using Engine = std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)>;
constexpr uint64_t base = 0x100000;

static HuskArm64CacheStats working_set(bool reclaim)
{
    setenv("HUSK_ARM64_CACHE_RECLAIM", reclaim ? "1" : "0", 1);
    Engine engine(husk_direct_create(2 * 1024 * 1024), husk_direct_destroy);
    Page code;
    code.fill(0x91000400); // add x0,x0,#1
    code.back() = 0xd4000001;
    auto run = [&](size_t page) {
        HuskArm64State state{};
        state.pc = base + page * 4096;
        CHECK(husk_direct_run(engine.get(), &state, state.pc, code.data(), {}, 4096) == HUSK_ARM64_FALLBACK);
        CHECK(state.x[0] == 1023 && state.pc == base + page * 4096 + 4092);
    };
    auto start = std::chrono::steady_clock::now();
    for (unsigned cold = 32; cold < 432; ++cold) {
        for (unsigned hot = 0; hot < 32; ++hot) run(hot);
        run(cold);
    }
    auto stats = husk_direct_cache_stats(engine.get());
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    CHECK(stats.bytes <= 2 * 1024 * 1024);
    CHECK(reclaim ? stats.evictions && !stats.flushes : stats.flushes && !stats.evictions);
    std::printf("cache reclaim=%d compiles=%llu evictions=%llu flushes=%llu us=%lld\n", reclaim,
                (unsigned long long)stats.compiles, (unsigned long long)stats.evictions,
                (unsigned long long)stats.flushes, (long long)elapsed);
    return stats;
}

struct Memory {
    std::vector<Page> code{128};
    alignas(16384) std::array<uint64_t, 2048> data{};
    bool readable = true;
    static const void *Code(void *opaque, uint64_t page) {
        auto &m = *static_cast<Memory *>(opaque);
        size_t index = (page - base) / 4096;
        return index < m.code.size() ? m.code[index].data() : nullptr;
    }
    static void *Data(void *opaque, uint64_t page, bool write) {
        auto &m = *static_cast<Memory *>(opaque);
        return !write && m.readable && page >= 0x20000 && page < 0x24000 ?
            reinterpret_cast<char *>(m.data.data()) + page - 0x20000 : nullptr;
    }
};

static void chained_reuse(bool fastmem, bool sharing = false)
{
    setenv("HUSK_ARM64_CODE_SHARING", sharing ? "1" : "0", 1);
    setenv("HUSK_ARM64_FASTMEM", fastmem ? "1" : "0", 1);
    setenv("HUSK_ARM64_CACHE_RECLAIM", "1", 1);
    setenv("HUSK_ARM64_LOOP_MAP", "0", 1);
    Engine engine(husk_direct_create(2 * 1024 * 1024), husk_direct_destroy);
    auto memory = std::make_unique<Memory>();
    auto &m = *memory;
    m.data[0] = 0x123456789abcdef0;
    for (unsigned page = 0; page < m.code.size(); ++page) {
        auto &code = m.code[page];
        code.fill(0xd4000001);
        for (unsigned i = 0; i < 512; i += 2) {
            code[i] = 0xf9400022; // ldr x2,[x1]
            code[i + 1] = 0x91000400;
        }
        if (page + 1 < m.code.size()) code[512] = 0x14000200;
    }
    HuskArm64Memory callbacks{&m, nullptr, nullptr, Memory::Data, nullptr, Memory::Code};
    for (unsigned round = 0; round < 3; ++round) {
        HuskArm64State state{};
        state.pc = base; state.x[1] = 0x20000;
        CHECK(husk_direct_run(engine.get(), &state, base, m.code[0].data(), callbacks, 100000) == HUSK_ARM64_FALLBACK);
        CHECK(state.x[0] == 256 * m.code.size());
        CHECK(state.x[2] == m.data[0] && state.pc == base + 127 * 4096 + 2048);
        auto stats = husk_direct_cache_stats(engine.get());
        CHECK(sharing ? stats.compiles == 2 && !stats.evictions : stats.evictions > 0);
    }
    // Recycled JIT addresses must use their new fault records. Permission
    // revocation also checks that a former native mapping cannot be reused.
    m.readable = false;
    HuskArm64State state{};
    state.pc = base; state.x[1] = 0x20000;
    CHECK(husk_direct_run(engine.get(), &state, base, m.code[0].data(), callbacks, 100000) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == base && !state.x[0] && !state.x[2]);
    m.readable = true;
    // Replace a cached page at the same VA and extend it from a different
    // entry. Local branches must never retain the previous page's code.
    auto &code = m.code[0];
    code.fill(0xd4000001); code[0] = 0x91000800;
    state = {}; state.pc = base;
    CHECK(husk_direct_run(engine.get(), &state, base, code.data(), callbacks, 1000) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == base + 4 && state.x[0] == 2);
    code[2] = 0x17fffffe;
    state = {}; state.pc = base + 8;
    CHECK(husk_direct_run(engine.get(), &state, base, code.data(), callbacks, 1000) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == base + 4 && state.x[0] == 2);
    auto stats = husk_direct_cache_stats(engine.get());
    CHECK(stats.bytes <= 2 * 1024 * 1024 && !stats.flushes);
    std::printf("cache chained reuse fastmem=%d sharing=%d compiles=%llu evictions=%llu PASS\n",
                fastmem, sharing, (unsigned long long)stats.compiles, (unsigned long long)stats.evictions);
}

static void local_return_table()
{
    Engine engine(husk_direct_create(2 * 1024 * 1024), husk_direct_destroy);
    Page code;
    code.fill(0xd4000001);
    code[0] = 0xd65f0060; // ret x3
    code[4] = 0x91000400;
    HuskArm64State state{};
    state.pc = base + 16;
    CHECK(husk_direct_run(engine.get(), &state, base, code.data(), {}, 128) == HUSK_ARM64_FALLBACK);
    for (unsigned i = 0; i < 32; ++i) {
        state = {}; state.pc = base; state.x[3] = base + 16;
        CHECK(husk_direct_run(engine.get(), &state, base, code.data(), {}, 128) == HUSK_ARM64_FALLBACK);
        CHECK(state.pc == base + 20 && state.x[0] == 1);
    }
    std::puts("cache stable local return table PASS");
}

static void shared_addresses(bool sharing)
{
    setenv("HUSK_ARM64_CODE_SHARING", sharing ? "1" : "0", 1);
    Engine engine(husk_direct_create(2 * 1024 * 1024), husk_direct_destroy);
    Page code;
    code.fill(0xd4000001);
    code[0] = 0x10000000; // adr x0,.
    code[1] = 0xb0000001; // adrp x1,.+4096
    code[2] = 0x94000002; // bl .+8
    code[4] = 0x910003c2; // add x2,x30,#0
    code[5] = 0xd65f03c0;
    auto started = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t address = i & 1 ? UINT64_C(0xffff800001000000) : UINT64_C(0x7f0001000000);
        address += i * 16384;
        HuskArm64State state{};
        state.pc = address;
        CHECK(husk_direct_run(engine.get(), &state, address, code.data(), {}, 128) == HUSK_ARM64_FALLBACK);
        CHECK(state.pc == address + 12 && state.x[0] == address && state.x[1] == address + 4096);
        CHECK(state.x[2] == address + 12 && state.x[30] == address + 12);
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    auto stats = husk_direct_cache_stats(engine.get());
    CHECK(stats.compiles == (sharing ? 1 : 512) && stats.shared_hits == (sharing ? 511 : 0));
    // Different bytes at one VA, followed by the original image at another VA,
    // must select their own code while preserving the reusable original image.
    code[0] = 0x10000080; // adr x0,.+16
    HuskArm64State state{};
    state.pc = base;
    CHECK(husk_direct_run(engine.get(), &state, base, code.data(), {}, 128) == HUSK_ARM64_FALLBACK);
    CHECK(state.x[0] == base + 16);
    code[0] = 0x10000000;
    state = {}; state.pc = base + 4096;
    CHECK(husk_direct_run(engine.get(), &state, state.pc, code.data(), {}, 128) == HUSK_ARM64_FALLBACK);
    CHECK(state.x[0] == base + 4096 && husk_direct_cache_stats(engine.get()).compiles == (sharing ? 2 : 514));
    std::printf("cache guest addresses sharing=%d compiles=%llu hits=%llu us=%lld PASS\n", sharing,
                (unsigned long long)stats.compiles, (unsigned long long)stats.shared_hits, (long long)elapsed);
}

static size_t stub_workload(bool shared, bool fastmem)
{
    setenv("HUSK_ARM64_SHARED_STUBS", shared ? "1" : "0", 1);
    setenv("HUSK_ARM64_FASTMEM", fastmem ? "1" : "0", 1);
    setenv("HUSK_ARM64_CODE_SHARING", "0", 1);
    auto memory = std::make_unique<Memory>();
    auto &m = *memory;
    m.code.resize(8);
    m.data[0] = 0xabcdef0123456789;
    for (size_t page = 0; page < m.code.size(); ++page) {
        auto &code = m.code[page];
        code.fill(0xd4000001);
        for (unsigned i = 0; i < 512; i += 2) {
            code[i] = 0xf9400022; // ldr x2,[x1]
            code[i + 1] = 0x91000400; // add x0,x0,#1
        }
        if (page + 1 < m.code.size()) code[512] = 0x14000200;
    }
    Engine engine(husk_direct_create(2 * 1024 * 1024), husk_direct_destroy);
    HuskArm64Memory callbacks{&m, nullptr, nullptr, Memory::Data, nullptr, Memory::Code};
    for (unsigned round = 0; round < 32; ++round) {
        HuskArm64State state{};
        state.pc = base; state.x[1] = 0x20000; state.nzcv = 0xb0000000;
        for (unsigned r = 3; r < 32; ++r) state.x[r] = UINT64_C(0xffeeddccbb000000) + r;
        CHECK(husk_direct_run(engine.get(), &state, base, m.code[0].data(), callbacks, 100000) == HUSK_ARM64_FALLBACK);
        CHECK(state.x[0] == 256 * m.code.size() && state.x[2] == m.data[0]);
        CHECK(state.pc == base + 7 * 4096 + 2048 && state.nzcv == 0xb0000000);
        for (unsigned r = 3; r < 32; ++r) CHECK(state.x[r] == UINT64_C(0xffeeddccbb000000) + r);
    }
    auto stats = husk_direct_cache_stats(engine.get());
    CHECK(!stats.evictions && stats.compiles == m.code.size());
    std::printf("cache shared stubs=%d fastmem=%d bytes=%zu PASS\n", shared, fastmem, stats.bytes);
    return stats.bytes;
}

int main()
{
    setenv("HUSK_ARM64_CODE_SHARING", "0", 1);
    auto flushed = working_set(false), reclaimed = working_set(true);
    CHECK(reclaimed.compiles == 432 && reclaimed.compiles < flushed.compiles);
    chained_reuse(false);
    chained_reuse(true);
    local_return_table();
    chained_reuse(false, true);
    chained_reuse(true, true);
    shared_addresses(false);
    shared_addresses(true);
    for (bool fastmem : {false, true}) {
        auto before = stub_workload(false, fastmem);
        auto after = stub_workload(true, fastmem);
        CHECK(after < before);
    }
}
