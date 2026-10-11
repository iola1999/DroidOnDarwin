/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

namespace
{
NceDirectState state(std::mt19937_64 &random, unsigned base = 0)
{
    NceDirectState value{};
    for (auto &reg : value.x)
        reg = random();
    for (auto &vector : value.v) {
        vector[0] = random();
        vector[1] = random();
    }
    value.pc = CodeVA;
    value.sp = DataVA + 512;
    if (base != 31)
        value.x[base] = value.sp;
    value.nzcv = random() & 0xf0000000;
    value.fpcr = (random() & 15) << 22;
    value.fpsr = random() & 0x0800009f;
    value.access_flags = NCE_DIRECT_UNALIGNED;
    value.exclusive_address = UINT64_MAX;
    return value;
}
void report(const char *name)
{
    std::printf("{\"test\":\"%s\",\"passed\":true}\n", name);
}
void *first_page(void *opaque, uint64_t address, bool write)
{
    return address >= DataVA + 4096 ? nullptr : Probe::Map(opaque, address, write);
}
}  // namespace

void direct_memory_group_tests(Probe &probe)
{
    auto original = probe.direct;
    auto config = probe.config;
    config.memory_groups = true;
    probe.direct = nce_direct_create(config);
    CHECK(probe.direct);
    std::mt19937_64 random(0x47524f5550);
    constexpr uint32_t scalar[] = {0x39000000, 0x39400000, 0x79000000, 0x79400000,
                                   0xb9000000, 0xb9400000, 0xf9000000, 0xf9400000,
                                   0xbd000000, 0xbd400000, 0xfd000000, 0xfd400000,
                                   0x3d800000, 0x3dc00000, 0x39800000, 0x39c00000,
                                   0x79800000, 0x79c00000, 0xb9800000};
    constexpr uint32_t pairs[] = {0x29000000, 0x29400000, 0xa9000000, 0xa9400000,
                                  0x2d000000, 0x2d400000, 0x6d000000,
                                  0x6d400000, 0xad000000, 0xad400000};
    for (unsigned base = 0; base < 32; ++base) {
        std::array<uint32_t, 5> words{};
        for (auto op : scalar) {
            for (unsigned j = 0; j < 4; ++j)
                words[j] = op | (j * 4 << 10) | (base << 5) | ((base + j + 1) % 32);
            for (auto &value : probe.data)
                value = random();
            probe.load(words.data(), sizeof(words));
            probe.compare(state(random, base), 4);
        }
        for (auto op : pairs) {
            for (bool negative : {false, true}) {
                for (unsigned j = 0; j < 4; ++j)
                    words[j] = op | (((j * 4 - (negative ? 8 : 0)) & 127) << 15) |
                               (((base + 2 * j + 2) % 32) << 10) | (base << 5) | ((base + 2 * j + 1) % 32);
                for (auto &value : probe.data)
                    value = random();
                probe.load(words.data(), sizeof(words));
                probe.compare(state(random, base), 4);
            }
        }
    }
    auto stats = nce_direct_stats(probe.direct);
    CHECK(stats.memory_groups >= 32 * (std::size(scalar) + 2 * std::size(pairs)));
    CHECK(stats.memory_group_instructions >= 4 * stats.memory_groups);
    report("memory_groups_all_bases_widths_vectors_and_pairs");

    for (bool write : {false, true}) {
        std::array<uint32_t, 5> words{};
        for (unsigned j = 0; j < 4; ++j)
            words[j] = (write ? 0xf9000000 : 0xf9400000) | (j << 10) | (j + 1);
        probe.load(words.data(), sizeof(words));
        auto initial = state(random);
        probe.compare(initial, 4);
        for (unsigned first = 1; first < 4; ++first) {
            initial.pc = CodeVA + first * 4;
            probe.compare(initial, 4 - first);
        }
        initial.pc = CodeVA;
        for (unsigned completed = 1; completed <= 4; ++completed) {
            initial.x[0] = DataVA + 4096 - completed * 8;
            auto actual = initial, expected = initial;
            auto expected_data = probe.data;
            for (unsigned j = 0; j < completed; ++j) {
                size_t slot = 512 - completed + j;
                if (write)
                    expected_data[slot] = expected.x[j + 1];
                else
                    expected.x[j + 1] = expected_data[slot];
            }
            expected.pc += completed * 4;
            uint64_t fault = 0;
            auto memory = probe.memory();
            memory.map_page = first_page;
            auto exit = nce_direct_run(probe.direct, &actual, memory, 1000, &fault);
            CHECK(exit == (completed == 4 ? NCE_DIRECT_UNSUPPORTED : write ? NCE_DIRECT_WRITE_FAULT
                                                                           : NCE_DIRECT_READ_FAULT));
            if (completed != 4)
                CHECK(fault == DataVA + 4096);
            CHECK(!std::memcmp(&actual, &expected, sizeof(actual)) && probe.data == expected_data);
        }
    }
    report("memory_groups_inner_entries_and_partial_page_faults");

    const uint32_t sp_stores[] = {0xf90003e1, 0xf90007e2, 0};
    probe.load(sp_stores, sizeof(sp_stores));
    auto initial = state(random), actual = initial;
    actual.sp = initial.sp = DataVA + 1;
    auto before_data = probe.data;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)) && probe.data == before_data);
    actual.sp = initial.sp = DataVA;
    probe.alias_code = true;
    ++probe.epoch;
    auto before_code = probe.code;
    uint64_t fault = 0;
    CHECK(probe.run(actual, 1000, &fault) == NCE_DIRECT_WRITE_FAULT && fault == DataVA);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)) && probe.code == before_code);
    probe.alias_code = false;
    ++probe.epoch;
    report("memory_groups_sp_alignment_and_code_write_alias");

    const uint32_t fallback[] = {0x69400801, 0x69421003, 0};
    probe.load(fallback, sizeof(fallback));
    actual = initial = state(random);
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
    const uint32_t base_change[] = {0xf9400000, 0xf9400001, 0};
    probe.load(base_change, sizeof(base_change));
    initial = state(random);
    probe.data[64] = DataVA + 1024;
    probe.compare(initial, 2);
    // A branch target inside adjacent loads must retain its own entry.
    const uint32_t branch[] = {0xb4000085, 0xf9400001, 0xf9400402, 0xf9400803, 0xf9400c04, 0};
    probe.load(branch, sizeof(branch));
    for (unsigned value : {0, 1}) {
        initial = state(random);
        initial.x[5] = value;
        probe.compare(initial, 5);
    }
    report("memory_groups_fallback_base_changes_and_branch_targets");

    const uint32_t loop[] = {0xf9400001, 0xf9400402, 0xf9400803, 0xf9400c04, 0x17fffffc};
    probe.load(loop, sizeof(loop));
    actual = state(random);
    std::atomic<bool> done{false};
    std::thread timer([&] {
        while (!done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            nce_direct_request_stop(probe.direct);
        }
    });
    CHECK(probe.run(actual, UINT64_C(1) << 60) == NCE_DIRECT_BUDGET && actual.pc == CodeVA);
    done.store(true, std::memory_order_release);
    timer.join();
    report("memory_groups_asynchronous_stop");
    nce_direct_destroy(probe.direct);
    probe.direct = original;
}
