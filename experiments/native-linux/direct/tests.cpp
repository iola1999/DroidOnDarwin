/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <random>
#include <atomic>
#include <thread>
#include <chrono>
static NceDirectState state(std::mt19937_64 &random)
{
    NceDirectState result{};
    for (auto &x : result.x)
        x = random();
    for (auto &v : result.v) {
        v[0] = random();
        v[1] = random();
    }
    result.sp = DataVA + 256;
    result.pc = CodeVA;
    result.nzcv = random() & 0xf0000000;
    result.access_flags = NCE_DIRECT_UNALIGNED | NCE_DIRECT_CRC;
    result.exclusive_address = UINT64_MAX;
    return result;
}
static void report(const char *name)
{
    std::printf("{\"test\":\"%s\",\"passed\":true}\n", name);
}
void direct_tests()
{
    auto holder = std::make_unique<Probe>();
    auto &probe = *holder;
    std::mt19937_64 random(0x4845534b);
    std::array<uint32_t, 65> words{};
    for (unsigned sample = 0; sample < 1024; ++sample) {
        for (unsigned i = 0; i < 64; ++i) {
            unsigned rd = i & 31, rn = (i + sample) & 31, rm = random() & 31;
            unsigned sf = random() & 1, op = random() & 3;
            switch ((i + sample) % 17) {
                case 0:
                    words[i] = (sf << 31) | (op << 29) | 0x11000000 |
                               ((random() & 4095) << 10) | (rn << 5) | rd;
                    break;
                case 1:
                    words[i] = (sf << 31) | (op << 29) | 0x0b000000 |
                               ((random() % 3) << 22) | (rm << 16) |
                               ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 2:
                    words[i] = (sf << 31) | (op << 29) | 0x0a000000 |
                               ((random() & 7) << 21) | (rm << 16) |
                               ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 3:
                    words[i] = (sf << 31) | ((op == 1 ? 2 : op) << 29) | 0x12800000 |
                               ((random() & (sf ? 3 : 1)) << 21) |
                               ((random() & 65535) << 5) | rd;
                    break;
                case 4:
                    words[i] = (sf << 31) | (op << 29) | 0x12000000 |
                               (sf << 22) | ((random() & (sf ? 63 : 31)) << 16) |
                               ((random() % (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 5:
                    words[i] = (sf << 31) | ((op % 3) << 29) | 0x13000000 |
                               (sf << 22) | ((random() & (sf ? 63 : 31)) << 16) |
                               ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 6:
                    words[i] = (sf << 31) | 0x13800000 | (sf << 22) | (rm << 16) |
                               ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 7: {
                    constexpr uint32_t ops[] = {0x5ac00000, 0x5ac00400, 0x5ac00800, 0x5ac01000, 0x5ac01400};
                    words[i] = (sf << 31) | ops[random() % 5] | (rn << 5) | rd;
                    break;
                }
                case 8: {
                    constexpr uint32_t ops[] = {0x1ac00800, 0x1ac00c00, 0x1ac02000, 0x1ac02400, 0x1ac02800, 0x1ac02c00};
                    words[i] = (sf << 31) | ops[random() % 6] | (rm << 16) | (rn << 5) | rd;
                    break;
                }
                case 9:
                    words[i] = (sf << 31) | ((random() & 1) << 30) | 0x1a800000 |
                               ((random() & 1) << 10) | ((random() & 15) << 12) |
                               (rm << 16) | (rn << 5) | rd;
                    break;
                case 10:
                    words[i] = (sf << 31) | 0x1b000000 | ((random() & 1) << 15) |
                               ((random() & 31) << 10) | (rm << 16) | (rn << 5) | rd;
                    break;
                case 11:
                    words[i] = 0xdac00c00 | (rn << 5) | rd;
                    break;
                case 12:
                    words[i] = (sf << 31) | 0x0b200000 | (op << 29) | (rm << 16) |
                               ((random() & 7) << 13) | ((random() % 5) << 10) | (rn << 5) | rd;
                    break;
                case 13:
                    words[i] = (sf << 31) | (op << 29) | 0x1a000000 |
                               (rm << 16) | (rn << 5) | rd;
                    break;
                case 14:
                    words[i] = (sf << 31) | ((random() & 1) << 30) | 0x3a400000 |
                               ((random() & 1) << 11) | ((random() & 15) << 12) |
                               (rm << 16) | (rn << 5) | (random() & 15);
                    break;
                case 15: {
                    constexpr uint32_t ops[] = {0x9b207c00, 0x9ba07c00, 0x9b407c00, 0x9bc07c00};
                    words[i] = ops[random() % 4] | (rm << 16) | (rn << 5) | rd;
                    break;
                }
                case 16: {
                    constexpr uint32_t ops[] = {0x1ac04000, 0x1ac04400, 0x1ac04800, 0x9ac04c00,
                                                0x1ac05000, 0x1ac05400, 0x1ac05800, 0x9ac05c00};
                    words[i] = ops[random() % 8] | (rm << 16) | (rn << 5) | rd;
                    break;
                }
            }
        }

        probe.load(words.data(), sizeof(words));
        probe.compare(state(random), 64);
    }
    report("integer_gpr_sp_nzcv_65536_instructions");
    for (unsigned base = 0; base < 31; ++base) {
        for (unsigned data = 0; data < 31; ++data) {
            if (base == data)
                continue;
            unsigned counter = 0;
            while (counter == base || counter == data)
                ++counter;
            const uint32_t code[] = {
                0xf9400000 | (base << 5) | data,
                0x91000400 | (data << 5) | data,
                0xf9000000 | (base << 5) | data,
                0xf1000400 | (counter << 5) | counter,
                0x54ffff81, 0};
            auto initial = state(random);
            initial.x[base] = DataVA;
            initial.x[counter] = 20;
            probe.data.fill(0);
            probe.load(code, sizeof(code));
            probe.compare(initial, 5);
        }
    }
    report("memory_all_gpr_930_combinations");
    for (unsigned sample = 0; sample < 512; ++sample) {
        auto initial = state(random);
        initial.fpcr = (random() & 15) << 22;
        initial.fpsr = random() & 0x0800009f;
        for (unsigned i = 0; i < 64; ++i) {
            unsigned rd = i & 31, rn = random() & 31, rm = random() & 31;
            unsigned operands = (rm << 16) | (rn << 5) | rd;
            switch ((i + sample) % 6) {
                case 0:
                    words[i] = 0x4e208400 | ((random() & 3) << 22) | operands;
                    break;
                case 1:
                    words[i] = 0x6e201c00 | operands;
                    break;
                case 2:
                    words[i] = 0x1e200800 | ((random() & 1) << 22) | ((random() & 7) << 12) | operands;
                    break;
                case 3:
                    words[i] = 0x9e670000 | (rn << 5) | rd;
                    break;
                case 4:
                    words[i] = 0x9e660000 | (rn << 5) | rd;
                    break;
                case 5:
                    words[i] = 0x4e20d400 | ((random() & 1) << 22) | operands;
                    break;
            }
        }
        probe.load(words.data(), sizeof(words));
        probe.compare(initial, 64);
    }
    report("fp_simd_rounding_fpsr_32768_instructions");
    const uint32_t state_code[] = {0xd53bd040, 0x91000652, 0xca120000, 0x4ee18400, 0};
    probe.load(state_code, sizeof(state_code));
    for (unsigned i = 0; i < 128; ++i) {
        auto initial = state(random);
        initial.tpidr_el0 = random();
        probe.compare(initial, 4);
    }
    report("tls_x18_vectors");
    const uint32_t store[] = {0xf8008401, 0};  // str x1,[x0],#8
    probe.load(store, sizeof(store));
    auto initial = state(random);
    initial.x[0] = DataVA;
    probe.writable = false;
    ++probe.epoch;
    auto actual = initial;
    auto before = probe.data;
    uint64_t fault;
    CHECK(probe.run(actual, 100, &fault) == NCE_DIRECT_WRITE_FAULT);
    CHECK(fault == DataVA && !std::memcmp(&initial, &actual, sizeof(initial)) && before == probe.data);
    probe.writable = true;
    ++probe.epoch;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && actual.pc == CodeVA + 4);
    CHECK(actual.x[0] == DataVA + 8 && probe.data[0] == initial.x[1]);
    report("write_permission_fault_and_post_index_retry");
    probe.alias_code = true;
    ++probe.epoch;
    actual = initial;
    auto code_before = probe.code;
    CHECK(probe.run(actual, 100, &fault) == NCE_DIRECT_WRITE_FAULT);
    CHECK(probe.code == code_before && actual.pc == initial.pc && actual.x[0] == initial.x[0]);
    probe.alias_code = false;
    ++probe.epoch;
    report("writable_physical_code_alias_rejected");
    const uint32_t load[] = {0xf8408401, 0};  // ldr x1,[x0],#8
    probe.load(load, sizeof(load));
    actual = initial;
    actual.x[0] = DataVA + 4092;
    auto cross = actual;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && !std::memcmp(&actual, &cross, sizeof(cross)));
    actual.x[0] = DataVA;
    probe.readable = false;
    ++probe.epoch;
    CHECK(probe.run(actual, 100, &fault) == NCE_DIRECT_READ_FAULT && fault == DataVA && actual.pc == CodeVA);
    probe.readable = true;
    ++probe.epoch;
    report("cross_page_and_read_permission_fault");
    const uint32_t first[] = {0xd2800220, 0};
    probe.load(first, sizeof(first));
    actual = initial;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && actual.x[0] == 17);
    probe.code[0] = 0xd2800a60;
    actual = initial;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && actual.x[0] == 83);
    report("changed_code_same_guest_address");
    for (uint32_t word : {0x14000000U, 0xd61f0000U, 0xd65f03c0U}) {
        probe.load(&word, 4);
        actual = initial;
        actual.x[0] = actual.x[30] = CodeVA;
        CHECK(probe.run(actual, 100) == NCE_DIRECT_BUDGET && actual.pc == CodeVA);
    }
    report("direct_and_indirect_self_branch_budget");
    // A return from a different page must preserve its register state and PC.
    const uint32_t branches[] = {0xd63f0040, 0xd503201f, 0};  // blr x2
    probe.load(branches, sizeof(branches));
    probe.code[1024] = 0x91000400;  // add x0,x0,#1
    probe.code[1025] = 0xd65f03c0;
    actual = initial;
    actual.x[2] = CodeVA + 4096;
    uint64_t value = actual.x[0];
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && actual.pc == CodeVA + 8 && actual.x[0] == value + 1);
    CHECK(actual.x[30] == CodeVA + 4);
    report("cross_page_indirect_call_return");
    probe.code[1024] = 0x91000800;  // add x0,x0,#2
    actual = initial;
    actual.x[2] = CodeVA + 4096;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && actual.x[0] == value + 2);
    report("changed_cross_page_branch_cache");
    const uint32_t zero[] = {0xd53b00e5, 0xd50b7423, 0xf9800060, 0};
    for (unsigned offset = 0; offset < 16384; offset += 37) {
        auto initial_zero = state(random);
        initial_zero.access_flags |= NCE_DIRECT_DC_ZVA64;
        initial_zero.x[3] = DataVA + offset;
        probe.data.fill(UINT64_MAX);
        probe.load(zero, sizeof(zero));
        probe.compare(initial_zero, 3);
    }
    actual = state(random);
    actual.access_flags |= NCE_DIRECT_DC_ZVA64;
    actual.pc += 4;
    actual.x[3] = DataVA + 47;
    auto expected = actual;
    probe.writable = false;
    ++probe.epoch;
    before = probe.data;
    CHECK(probe.run(actual, 100, &fault) == NCE_DIRECT_WRITE_FAULT && fault == DataVA);
    CHECK(!std::memcmp(&expected, &actual, sizeof(actual)) && before == probe.data);
    probe.writable = true;
    ++probe.epoch;
    report("dczid_zero_block_alignment_permission_and_prefetch");
    for (unsigned sample = 0; sample < 512; ++sample) {
        unsigned rn = sample & 31, rt = (sample >> 5) & 31, rt2 = (rt + 1) & 31;
        if (rn == rt || rn == rt2)
            continue;
        unsigned wide = (sample >> 1) & 1;
        unsigned scaled = wide ? 2 : 4;
        const uint32_t pair[] = {
            (wide << 31) | 0x29800000 | (((-scaled) & 127) << 15) | (rt2 << 10) | (rn << 5) | rt,
            0xd2800000 | rt, 0xd2800000 | rt2,
            (wide << 31) | 0x28c00000 | (scaled << 15) | (rt2 << 10) | (rn << 5) | rt, 0};
        auto pair_state = state(random);
        if (rn != 31)
            pair_state.x[rn] = pair_state.sp;
        probe.load(pair, sizeof(pair));
        probe.compare(pair_state, 4);
    }
    report("pair_pre_post_index_all_base_registers");
    for (unsigned width = 0; width < 4; ++width) {
        const uint32_t atomic[] = {(width << 30) | 0x085ffc01,  // ldaxr rt=1,rn=0
                                   0x91000421,  // add x1,x1,#1
                                   (width << 30) | 0x0802fc01,  // stlxr rs=2,rt=1,rn=0
                                   0};
        for (unsigned sample = 0; sample < 128; ++sample) {
            auto atomic_state = state(random);
            atomic_state.x[0] = DataVA;
            probe.data[0] = random();
            uint64_t old = probe.data[0], mask = UINT64_MAX >> (64 - (8U << width));
            probe.load(atomic, sizeof(atomic));
            CHECK(probe.run(atomic_state) == NCE_DIRECT_UNSUPPORTED && atomic_state.pc == CodeVA + 12);
            CHECK(atomic_state.x[1] == (old & mask) + 1 && atomic_state.x[2] == 0);
            CHECK(probe.data[0] == ((old & ~mask) | ((old + 1) & mask)));
        }
    }
    report("exclusive_load_store_all_widths");
    for (bool middle : {false, true}) {
        uint32_t forever = middle ? 0xd503201f : 0x14000000;
        probe.load(&forever, 4);
        if (middle)
            probe.code[1] = 0xd65f03c0;
        actual = initial;
        actual.x[30] = CodeVA + 4;
        std::atomic<bool> done{false};
        std::thread timer([&] {
            while (!done.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                nce_direct_request_stop(probe.direct);
            }
        });
        CHECK(probe.run(actual, UINT64_C(1) << 60) == NCE_DIRECT_BUDGET);
        CHECK(actual.pc == CodeVA + (middle ? 4 : 0));
        done.store(true, std::memory_order_release);
        timer.join();
    }
    report("asynchronous_stop_at_block_and_indirect_boundary");
    auto large_page = probe.memory();
    large_page.page_bits = 14;
    probe.load(load, sizeof(load));
    actual = initial;
    actual.x[0] = DataVA + 4092;
    expected = actual;
    probe.reference = probe.data;
    probe.reference_run(expected, CodeVA + 4);
    CHECK(nce_direct_run(probe.direct, &actual, large_page, 1000, &fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&expected, &actual, sizeof(actual)));
    actual = initial;
    actual.x[0] = DataVA + 16380;
    expected = actual;
    CHECK(nce_direct_run(probe.direct, &actual, large_page, 1000, &fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&expected, &actual, sizeof(actual)));
    report("16k_data_page_internal_and_external_boundaries");
    for (unsigned bits : {12U, 14U}) {
        const uint32_t publish[] = {0xf9000001, 0xd61f0040};  // str x1,[x0]; br x2
        probe.load(publish, sizeof(publish));
        probe.alias_code = true;
        probe.alias_offset = size_t(1) << bits;
        probe.code[probe.alias_offset / 4] = 0xf9000003;  // str x3,[x0]
        actual = state(random);
        actual.x[0] = DataVA + 128;
        actual.x[1] = 17;
        actual.x[2] = CodeVA + probe.alias_offset;
        actual.x[3] = 29;
        auto memory = probe.memory();
        memory.page_bits = bits;
        CHECK(nce_direct_run(probe.direct, &actual, memory, 1000, &fault) == NCE_DIRECT_WRITE_FAULT);
        CHECK(actual.pc == CodeVA + probe.alias_offset && fault == DataVA + 128);
        uint64_t written;
        std::memcpy(&written, (char *)probe.code.data() + probe.alias_offset + 128, sizeof(written));
        CHECK(written == 17);
        probe.alias_code = false;
        probe.alias_offset = 0;
        ++probe.epoch;
    }
    report("cached_write_alias_becomes_executable");
    const uint32_t collision[] = {0xd63f0040, 0xf1000421, 0x54ffffc1, 0};
    probe.load(collision, sizeof(collision));
    probe.code[1024] = 0x91000400;
    probe.code[1025] = 0xd65f03c0;
    probe.extra_code_va = CodeVA + 0x10000000;
    actual = state(random);
    actual.x[0] = 7;
    actual.x[1] = 16;
    actual.x[2] = probe.extra_code_va;
    CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && actual.pc == CodeVA + 12 && actual.x[0] == 23);
    if (probe.config.reuse_code_checks)
        CHECK(nce_direct_stats(probe.direct).code_validation_hits > 0);
    probe.executable = false;
    actual.pc = CodeVA;
    CHECK(probe.run(actual, 1000, &fault) == NCE_DIRECT_FETCH_FAULT && fault == CodeVA && actual.pc == CodeVA);
    probe.executable = true;
    probe.extra_code_va = 0;
    report("route_collision_reuse_and_next_run_fetch_permission");
    nce_direct_destroy(probe.direct);
    probe.config.cache_bytes = 2 * 1024 * 1024;
    probe.direct = nce_direct_create(probe.config);
    CHECK(probe.direct);
    words.fill(0x91000400);  // add x0,x0,#1
    words[64] = 0;
    for (unsigned sample = 0; sample < 8192; ++sample) {
        words[0] = 0xd2800000 | (sample << 5);
        probe.load(words.data(), sizeof(words));
        actual = initial;
        CHECK(probe.run(actual) == NCE_DIRECT_UNSUPPORTED && actual.pc == CodeVA + 256);
        CHECK(actual.x[0] == sample + 63);
    }
    auto cache = nce_direct_stats(probe.direct);
    CHECK(cache.flushes > 0 && cache.code_bytes <= cache.code_capacity);
    report("bounded_cache_reuse_and_code_replacement");
    direct_page_table_tests();
}
