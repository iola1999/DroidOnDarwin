/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <algorithm>
#include <time.h>
#include <vector>
extern "C" {
#include "../translation/linux-memory.h"
#include "../translation/read-view.h"
extern uint64_t translation_integer(uint64_t *, uint64_t), translation_integer_end[];
extern uint64_t translation_memory(uint64_t *, uint64_t), translation_memory_end[];
extern uint64_t translation_atomic(uint64_t *, uint64_t), translation_atomic_end[];
}
static uint64_t now(clockid_t clock)
{
    struct timespec t;
    CHECK(!clock_gettime(clock, &t));
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
void direct_benchmark(const char *engine)
{
    auto probe = std::make_unique<Probe>(engine);
    struct Kernel {
        const char *name;
        uint64_t (*run)(uint64_t *, uint64_t);
        const void *end;
        uint64_t iterations;
    };
    const Kernel kernels[] = {{"integer", translation_integer, translation_integer_end, 64000000},
                              {"memory", translation_memory, translation_memory_end, 32000000},
                              {"atomic", translation_atomic, translation_atomic_end, 8000000}};
    for (auto kernel : kernels) {
        translation_memory_phase(kernel.name);
        size_t length = uintptr_t(kernel.end) - uintptr_t(kernel.run);
        probe->load((const void *)kernel.run, length);
        auto initialize = [&] {
            for (size_t i = 0; i < probe->data.size(); ++i)
                probe->data[i] = probe->reference[i] = i + 17;
        };
        auto execute = [&](uint64_t iterations) {
            NceDirectState state{};
            state.pc = CodeVA;
            state.x[0] = DataVA;
            state.x[1] = iterations;
            state.x[30] = CodeVA + length;
            state.access_flags = NCE_DIRECT_UNALIGNED;
            state.exclusive_address = UINT64_MAX;
            if (!std::strcmp(engine, "direct")) {
                NceDirectExit result;
                do {
                    result = probe->run(state, 1000000);
                } while (result == NCE_DIRECT_BUDGET);
                CHECK(result == NCE_DIRECT_UNSUPPORTED && state.pc == CodeVA + length);
            } else if (!std::strcmp(engine, "unicorn")) {
                probe->reference_run(state, CodeVA + length);
            } else
                state.x[0] = kernel.run(probe->data.data(), iterations);
            return state.x[0];
        };
        initialize();
        execute(64);
        for (unsigned round = 0; round < 3; ++round) {
            initialize();
            uint64_t cpu = now(CLOCK_THREAD_CPUTIME_ID), wall = now(CLOCK_MONOTONIC);
            uint64_t result = execute(kernel.iterations);
            wall = now(CLOCK_MONOTONIC) - wall;
            cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
            auto expected = std::make_unique<std::array<uint64_t, DataBytes / 8>>();
            for (size_t i = 0; i < expected->size(); ++i)
                (*expected)[i] = i + 17;
            uint64_t native = kernel.run(expected->data(), kernel.iterations);
            CHECK(result == native);
            CHECK(*expected == (!std::strcmp(engine, "unicorn") ? probe->reference : probe->data));
            auto stats = nce_direct_stats(probe->direct);
            std::printf("{\"kernel\":\"%s\",\"engine\":\"%s\",\"round\":%u,\"iterations\":%llu,\"wall_ns\":%llu,\"thread_cpu_ns\":%llu,\"code_bytes\":%zu,\"compiles\":%llu,\"mapping_misses\":%llu,\"passed\":true}\n",
                        kernel.name, engine, round, kernel.iterations, wall, cpu, stats.code_bytes, stats.compiles, stats.mapping_misses);
        }
    }
    translation_memory_phase("cleanup");
    probe.reset();
}

namespace
{
constexpr size_t MemoryPageBytes = 16384;
constexpr uint64_t SharedVA = 0x40000;
constexpr size_t MemoryCacheBytes = 128 * 1024 * 1024;
struct MemoryBenchmark {
    // Both address spaces use the same code and shared page, with four private
    // pages each. Switching the active table leaves every backing page intact.
    alignas(MemoryPageBytes) std::array<uint64_t, 10 * MemoryPageBytes / 8> ram{};
    alignas(MemoryPageBytes) std::array<uint64_t, 2048> root{};
    std::array<std::vector<nce_direct_flat_page>, 2> flat;
    std::array<NceReadView, 2> views{};
    NceDirectPageTable table{root.data(), CodeVA, UINT64_C(1) << 32, 0x500000000, ram.data(), sizeof(ram)};
    NceDirect *engine = nullptr;
    unsigned active = 0;
    uint64_t mapping_epoch = 1, code_epoch = 1;
    size_t code_size = 0;
    explicit MemoryBenchmark(const char *mode, bool check_only)
    {
        bool read_view = !std::strcmp(mode, "view");
        CHECK(!std::strcmp(mode, "native") || !std::strcmp(mode, "cached") || !std::strcmp(mode, "flat") || read_view);
        if (!std::strcmp(mode, "flat") || read_view) {
            for (unsigned context = 0; context < 2; ++context) {
                auto &pages = flat[context];
                pages.resize(DIRECT_FLAT_BYTES / sizeof(nce_direct_flat_page));
                pages[0] = {uintptr_t(ram.data()), 0};
                for (size_t page = 0; page < DataBytes / MemoryPageBytes; ++page) {
                    uintptr_t pointer = uintptr_t(data(context) + page * MemoryPageBytes / 8);
                    pages[((DataVA - CodeVA) >> 14) + page] = {pointer, pointer};
                }
                pages[(SharedVA - CodeVA) >> 14] = {uintptr_t(shared()), uintptr_t(shared())};
            }
        }
        if (std::strcmp(mode, "native")) {
            engine = nce_direct_create({MemoryCacheBytes, 256, 256, false, false, true, false, false, false, true, false, false});
            CHECK(engine);
        }
        std::printf("{\"memory_benchmark_config\":true,\"backend\":\"%s\",\"page_bits\":14,\"contexts\":2,\"private_bytes\":65536,\"shared_bytes\":16384,\"cache_bytes\":%zu,\"mapping_entries\":256,\"route_entries\":256,\"cross_page\":true,\"native_exclusive\":true,\"code_versions\":\"fixture\",\"check_scope\":\"independent\"", mode, engine ? MemoryCacheBytes : 0);
        if (read_view)
            std::printf(",\"read_memory\":\"premapped_view\",\"write_memory\":\"flat\",\"view_reserved_bytes\":%llu,\"view_mapped_bytes\":%zu", 2 * table.user_size, 12 * MemoryPageBytes);
        std::puts("}");
        if (read_view) {
            uint64_t cpu = check_only ? 0 : now(CLOCK_THREAD_CPUTIME_ID);
            uint64_t wall = check_only ? 0 : now(CLOCK_MONOTONIC);
            CHECK(!nce_direct_install_read_faults());
            for (unsigned context = 0; context < views.size(); ++context) {
                auto &view = views[context];
                CHECK(!nce_read_view_create(&view, table.user_size));
                CHECK(!nce_read_view_map(&view, 0, ram.data(), MemoryPageBytes));
                for (size_t page = 0; page < DataBytes / MemoryPageBytes; ++page)
                    CHECK(!nce_read_view_map(&view, DataVA - CodeVA + page * MemoryPageBytes,
                                             data(context) + page * MemoryPageBytes / 8, MemoryPageBytes));
                CHECK(!nce_read_view_map(&view, SharedVA - CodeVA, shared(), MemoryPageBytes));
            }
            if (!check_only) {
                wall = now(CLOCK_MONOTONIC) - wall;
                cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
            }
            std::printf("{\"memory_benchmark_view_setup\":true,\"check_only\":%s,\"maps\":%llu,\"revocations\":0,\"reserved_bytes\":%llu,\"mapped_bytes\":%zu",
                        check_only ? "true" : "false", views[0].maps + views[1].maps, 2 * table.user_size, 12 * MemoryPageBytes);
            if (!check_only)
                std::printf(",\"wall_ns\":%llu,\"thread_cpu_ns\":%llu", wall, cpu);
            std::puts("}");
        }
    }
    ~MemoryBenchmark()
    {
        nce_direct_destroy(engine);
        if (views[0].base) {
            uint64_t maps = views[0].maps + views[1].maps;
            uint64_t revocations = views[0].revocations + views[1].revocations;
            for (auto &view : views)
                CHECK(!nce_read_view_destroy(&view) && !view.base && !view.size);
            CHECK(!nce_direct_remove_read_faults());
            std::printf("{\"memory_benchmark_view_cleanup\":true,\"maps\":%llu,\"revocations\":%llu,\"destroyed\":2,\"live_reserved_bytes\":0,\"passed\":true}\n", maps, revocations);
        }
    }
    uint64_t *data(unsigned context)
    {
        return ram.data() + (1 + context * 4) * MemoryPageBytes / 8;
    }
    uint64_t *shared()
    {
        return ram.data() + 9 * MemoryPageBytes / 8;
    }
    void select(unsigned context)
    {
        CHECK(context < 2);
        if (active != context) {
            active = context;
            ++mapping_epoch;
        }
    }
    void load(const void *code, size_t size)
    {
        CHECK(size <= MemoryPageBytes);
        std::memset(ram.data(), 0, MemoryPageBytes);
        std::memcpy(ram.data(), code, size);
        code_size = size;
        ++code_epoch;
    }
    static void *map(void *opaque, uint64_t address, bool write)
    {
        auto &self = *static_cast<MemoryBenchmark *>(opaque);
        if (!write && address >= CodeVA && address < CodeVA + MemoryPageBytes)
            return reinterpret_cast<char *>(self.ram.data()) + address - CodeVA;
        if (address >= DataVA && address < DataVA + DataBytes)
            return reinterpret_cast<char *>(self.data(self.active)) + address - DataVA;
        if (address >= SharedVA && address < SharedVA + MemoryPageBytes)
            return reinterpret_cast<char *>(self.shared()) + address - SharedVA;
        return nullptr;
    }
    static const void *code(void *opaque, uint64_t address)
    {
        auto &self = *static_cast<MemoryBenchmark *>(opaque);
        return address >= CodeVA && address < CodeVA + MemoryPageBytes
                   ? reinterpret_cast<const char *>(self.ram.data()) + address - CodeVA
                   : nullptr;
    }
    static uint64_t version(void *opaque, const void *)
    {
        return static_cast<MemoryBenchmark *>(opaque)->code_epoch;
    }
    uint64_t execute(uint64_t (*native)(uint64_t *, uint64_t), uint64_t iterations, uint64_t address = DataVA)
    {
        if (!engine)
            return native(static_cast<uint64_t *>(map(this, address, true)), iterations);
        NceDirectState state{};
        state.pc = CodeVA;
        state.x[0] = address;
        state.x[1] = iterations;
        state.x[30] = CodeVA + code_size;
        state.access_flags = NCE_DIRECT_UNALIGNED;
        state.exclusive_address = UINT64_MAX;
        auto &pages = flat[active];
        NceDirectMemory memory{this, map, code, mapping_epoch, 14, pages.empty() ? nullptr : &table,
                               false, pages.empty() ? nullptr : pages.data(), version, code_epoch, views[active].base, nullptr};
        uint64_t fault = 0;
        NceDirectExit result;
        do {
            result = nce_direct_run(engine, &state, memory, 1000000, &fault);
        } while (result == NCE_DIRECT_BUDGET);
        if (result != NCE_DIRECT_UNSUPPORTED || state.pc != CodeVA + code_size)
            std::fprintf(stderr, "memory benchmark exit=%u pc=%llx expected=%llx fault=%llx context=%u\n", result, state.pc, CodeVA + code_size, fault, active);
        CHECK(result == NCE_DIRECT_UNSUPPORTED && state.pc == CodeVA + code_size);
        return state.x[0];
    }
    void check_contexts()
    {
        load(reinterpret_cast<const void *>(translation_memory), uintptr_t(translation_memory_end) - uintptr_t(translation_memory));
        std::array<std::array<uint64_t, DataBytes / 8>, 2> expected;
        std::array<uint64_t, MemoryPageBytes / 8> expected_shared;
        for (unsigned context = 0; context < 2; ++context)
            for (size_t i = 0; i < expected[context].size(); ++i)
                expected[context][i] = data(context)[i] = 37 + context * 54 + i;
        for (size_t i = 0; i < expected_shared.size(); ++i)
            expected_shared[i] = shared()[i] = 100 + i;
        for (unsigned round = 0; round < 4; ++round) {
            unsigned context = round % 2;
            select(context);
            CHECK(execute(translation_memory, 8192) == translation_memory(expected[context].data(), 8192));
            for (unsigned index = 0; index < 2; ++index)
                CHECK(std::equal(expected[index].begin(), expected[index].end(), data(index)));
            CHECK(execute(translation_memory, 64, SharedVA) == translation_memory(expected_shared.data(), 64));
            CHECK(std::equal(expected_shared.begin(), expected_shared.end(), shared()));
        }
        select(0);
        std::puts("{\"test\":\"memory_benchmark_same_va_private_shared_switch\",\"passed\":true}");
    }
};
}  // namespace

void direct_memory_benchmark(const char *memory, bool check_only)
{
    auto fixture = std::make_unique<MemoryBenchmark>(memory, check_only);
    translation_memory_phase("boundaries");
    fixture->check_contexts();
    struct Kernel {
        const char *name;
        uint64_t (*run)(uint64_t *, uint64_t);
        const void *end;
        uint64_t iterations, check_iterations;
    };
    const Kernel kernels[] = {{"integer", translation_integer, translation_integer_end, 64000000, 64},
                              {"memory", translation_memory, translation_memory_end, 32000000, 8192}};
    for (auto kernel : kernels) {
        translation_memory_phase(kernel.name);
        fixture->load(reinterpret_cast<const void *>(kernel.run), uintptr_t(kernel.end) - uintptr_t(kernel.run));
        auto initialize = [&] {
            for (size_t i = 0; i < DataBytes / 8; ++i)
                fixture->data(0)[i] = i + 17;
        };
        initialize();
        fixture->execute(kernel.run, kernel.check_iterations);
        for (unsigned round = 0; round < (check_only ? 1U : 3U); ++round) {
            initialize();
            uint64_t iterations = check_only ? kernel.check_iterations : kernel.iterations;
            uint64_t cpu = 0, wall = 0;
            if (!check_only) {
                cpu = now(CLOCK_THREAD_CPUTIME_ID);
                wall = now(CLOCK_MONOTONIC);
            }
            uint64_t result = fixture->execute(kernel.run, iterations);
            if (!check_only) {
                wall = now(CLOCK_MONOTONIC) - wall;
                cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
            }
            std::array<uint64_t, DataBytes / 8> expected;
            for (size_t i = 0; i < expected.size(); ++i)
                expected[i] = i + 17;
            CHECK(result == kernel.run(expected.data(), iterations));
            CHECK(std::equal(expected.begin(), expected.end(), fixture->data(0)));
            auto stats = nce_direct_stats(fixture->engine);
            std::printf("{\"memory_kernel\":\"%s\",\"backend\":\"%s\",\"check_only\":%s,\"round\":%u,\"iterations\":%llu,\"code_bytes\":%zu,\"compiles\":%llu,\"mapping_misses\":%llu,\"passed\":true",
                        kernel.name, memory, check_only ? "true" : "false", round, iterations, stats.code_bytes, stats.compiles, stats.mapping_misses);
            if (fixture->views[0].base)
                std::printf(",\"read_view_sites\":%llu,\"read_view_faults\":%llu,\"view_maps\":%llu,\"view_revocations\":%llu",
                            stats.read_view_sites, stats.read_view_faults,
                            fixture->views[0].maps + fixture->views[1].maps, fixture->views[0].revocations + fixture->views[1].revocations);
            if (!check_only)
                std::printf(",\"wall_ns\":%llu,\"thread_cpu_ns\":%llu", wall, cpu);
            std::puts("}");
        }
    }
    translation_memory_phase("cleanup");
    fixture.reset();
}
