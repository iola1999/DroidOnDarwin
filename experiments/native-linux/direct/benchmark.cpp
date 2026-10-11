/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <time.h>
extern "C" {
#include "../translation/linux-memory.h"
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
