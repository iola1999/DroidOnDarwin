/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

extern "C" {
#include "../translation/linux-memory.h"
extern uint64_t translation_memory(uint64_t *, uint64_t), translation_memory_end[];
extern uint64_t translation_state(uint64_t *, uint64_t), translation_state_end[];
}

namespace
{
constexpr size_t PageBytes = 16384;
constexpr uint64_t SharedVA = 0x40000;
constexpr uint64_t CollidingCodeVA = CodeVA + 0x100000;
struct Ram {
    alignas(PageBytes) std::array<unsigned char, 10 * PageBytes> bytes{};
    uint64_t *shared()
    {
        return reinterpret_cast<uint64_t *>(bytes.data() + 9 * PageBytes);
    }
    void code(const void *source, size_t size)
    {
        CHECK(size <= PageBytes);
        std::memset(bytes.data(), 0, PageBytes);
        std::memcpy(bytes.data(), source, size);
    }
};
struct Executor {
    Ram &ram;
    alignas(PageBytes) std::array<uint64_t, 2048> root{};
    std::vector<nce_direct_flat_page> flat;
    NceDirect *engine;
    NceDirectPageTable table;
    explicit Executor(Ram &memory, unsigned cpu, bool native = true, bool crossing = false)
        : ram(memory), flat(DIRECT_FLAT_BYTES / sizeof(nce_direct_flat_page)), engine(nce_direct_create({2 * 1024 * 1024, 256, 256, false, false, crossing, false, false, false, native, false, false})), table{root.data(), CodeVA, UINT64_C(1) << 32, 0x500000000, ram.bytes.data(), ram.bytes.size()}
    {
        CHECK(engine && cpu < 2);
        flat[0] = {uintptr_t(ram.bytes.data()), 0};
        for (unsigned page = 0; page < 4; ++page) {
            uintptr_t address = uintptr_t(ram.bytes.data() + (1 + cpu * 4 + page) * PageBytes);
            flat[((DataVA - CodeVA) >> 14) + page] = {address, address};
        }
        auto shared = uintptr_t(ram.shared());
        flat[(SharedVA - CodeVA) >> 14] = {shared, shared};
    }
    ~Executor()
    {
        nce_direct_destroy(engine);
    }
    NceDirectMemory memory()
    {
        return {this, map, code, 0, 14, &table, false, flat.data(), nullptr, 0, nullptr, nullptr};
    }
    static void *map(void *opaque, uint64_t address, bool write)
    {
        auto &self = *static_cast<Executor *>(opaque);
        if (address < CodeVA || address - CodeVA >= self.table.user_size)
            return nullptr;
        auto entry = self.flat[(address - CodeVA) >> 14];
        return reinterpret_cast<void *>(write ? entry.write : entry.read);
    }
    static const void *code(void *opaque, uint64_t address)
    {
        auto &self = *static_cast<Executor *>(opaque);
        if (address == CollidingCodeVA)
            return self.ram.bytes.data() + 4096;
        return address >= CodeVA && address < CodeVA + PageBytes ? self.ram.bytes.data() + address - CodeVA : nullptr;
    }
    NceDirectExit run(NceDirectState &state, uint64_t budget = UINT64_C(1) << 60)
    {
        uint64_t fault = 0;
        return nce_direct_run(engine, &state, memory(), budget, &fault);
    }
};
uint64_t nanoseconds(clockid_t clock)
{
    timespec value{};
    CHECK(!clock_gettime(clock, &value));
    return uint64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
}
NceDirectState state(uint64_t count = 0)
{
    NceDirectState value{};
    value.pc = CodeVA;
    value.x[0] = DataVA;
    value.x[1] = count;
    value.exclusive_address = UINT64_MAX;
    return value;
}
void report(const char *name)
{
    std::printf("{\"test\":\"%s\",\"passed\":true}\n", name);
}
template <class F>
void parallel(F body)
{
    std::atomic<unsigned> ready{0};
    std::array<std::thread, 2> threads;
    for (unsigned cpu = 0; cpu < 2; ++cpu)
        threads[cpu] = std::thread([&, cpu] {
            ready.fetch_add(1, std::memory_order_release);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            body(cpu);
        });
    for (auto &thread : threads)
        thread.join();
}
}  // namespace

void direct_parallel_tests()
{
    translation_memory_phase("parallel_execution");
    auto ram = std::make_unique<Ram>();
    std::array<std::unique_ptr<Executor>, 2> cpu;
    for (unsigned i = 0; i < 2; ++i)
        cpu[i] = std::make_unique<Executor>(*ram, i);

    size_t length = uintptr_t(translation_memory_end) - uintptr_t(translation_memory);
    ram->code(reinterpret_cast<void *>(translation_memory), length);
    std::array<std::array<uint64_t, DataBytes / 8>, 2> expected{};
    std::array<uint64_t, 2> returns{}, start{}, end{}, duration{};
    constexpr uint64_t Iterations = 4000000;
    for (unsigned i = 0; i < 2; ++i) {
        expected[i].fill(i + 7);
        std::memcpy(ram->bytes.data() + (1 + i * 4) * PageBytes, expected[i].data(), DataBytes);
        returns[i] = translation_memory(expected[i].data(), Iterations);
    }
    parallel([&](unsigned i) {
        auto value = state(Iterations);
        value.x[30] = CodeVA + length;
        start[i] = nanoseconds(CLOCK_MONOTONIC);
        auto begin = nanoseconds(CLOCK_THREAD_CPUTIME_ID);
        CHECK(cpu[i]->run(value) == NCE_DIRECT_UNSUPPORTED && value.pc == CodeVA + length);
        duration[i] = nanoseconds(CLOCK_THREAD_CPUTIME_ID) - begin;
        end[i] = nanoseconds(CLOCK_MONOTONIC);
        CHECK(value.x[0] == returns[i]);
        CHECK(!std::memcmp(ram->bytes.data() + (1 + i * 4) * PageBytes, expected[i].data(), DataBytes));
    });
    CHECK(std::max(start[0], start[1]) < std::min(end[0], end[1]));
    std::printf("{\"parallel_private_memory\":true,\"iterations_per_cpu\":%llu,\"wall_ns\":%llu,\"thread_cpu_ns\":[%llu,%llu],\"overlap_ns\":%llu}\n",
                Iterations, std::max(end[0], end[1]) - std::min(start[0], start[1]), duration[0], duration[1],
                std::min(end[0], end[1]) - std::max(start[0], start[1]));
    report("parallel_private_same_va_compile_and_memory");

    for (unsigned width = 0; width < 4; ++width) {
        for (uint32_t ordered : {0U, 0x8000U}) {
            const uint32_t words[] = {(width << 30) | 0x085f7c03 | ordered, 0x91000463,
                                      (width << 30) | 0x08027c03 | ordered, 0x35ffffa2,
                                      0xd1000421, 0xb5ffff61, 0};
            ram->code(words, sizeof(words));
            *ram->shared() = UINT64_C(0xaaaaaaaa00000000);
            parallel([&](unsigned i) {
                auto value = state(20000);
                value.x[0] = SharedVA;
                CHECK(cpu[i]->run(value) == NCE_DIRECT_UNSUPPORTED && value.pc == CodeVA + 24);
                CHECK(value.x[1] == 0 && value.x[2] == 0 && value.exclusive_address == UINT64_MAX);
            });
            uint64_t mask = UINT64_MAX >> (64 - (8U << width));
            CHECK(*ram->shared() == ((UINT64_C(0xaaaaaaaa00000000) & ~mask) |
                                     ((UINT64_C(0xaaaaaaaa00000000) + 40000) & mask)));
        }
    }
    report("parallel_native_exclusive_all_widths_and_orderings");

    length = uintptr_t(translation_state_end) - uintptr_t(translation_state);
    ram->code(reinterpret_cast<void *>(translation_state), length);
    parallel([&](unsigned i) {
        auto value = state();
        value.x[30] = CodeVA + length;
        value.x[18] = 37 + i;
        value.tpidr_el0 = DataVA + 16 * i;
        value.v[0][0] = i + 101;
        value.v[0][1] = i + 103;
        value.v[1][0] = 107;
        value.v[1][1] = 109;
        CHECK(cpu[i]->run(value) == NCE_DIRECT_UNSUPPORTED);
        CHECK(value.x[18] == 38 + i && value.x[0] == ((DataVA + 16 * i) ^ (38 + i)));
        CHECK(value.v[0][0] == i + 208 && value.v[0][1] == i + 212);
    });
    report("parallel_tls_x18_vectors_and_replaced_code");

    const uint32_t loop[] = {0x91000400, 0x17ffffff};
    ram->code(loop, sizeof(loop));
    std::array<std::atomic<bool>, 2> entered{}, done{};
    std::array<std::thread, 2> threads;
    for (unsigned i = 0; i < 2; ++i)
        threads[i] = std::thread([&, i] {
            auto value = state();
            entered[i].store(true, std::memory_order_release);
            CHECK(cpu[i]->run(value) == NCE_DIRECT_BUDGET && value.pc == CodeVA);
            done[i].store(true, std::memory_order_release);
        });
    while (!entered[0].load(std::memory_order_acquire) || !entered[1].load(std::memory_order_acquire))
        std::this_thread::yield();
    for (unsigned i = 0; i < 2; ++i) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done[i].load(std::memory_order_acquire)) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            nce_direct_request_stop(cpu[i]->engine);
        }
        if (!i)
            CHECK(!done[1].load(std::memory_order_acquire));
        threads[i].join();
    }
    report("parallel_targeted_stop");

    for (bool native : {false, true}) {
        Executor candidate(*ram, 0, native);
        std::array<uint32_t, 34> split{};
        split[31] = 0xc85ffc03;
        split[32] = 0xc802fc03;
        ram->code(split.data(), sizeof(split));
        *ram->shared() = 17;
        auto value = state();
        value.pc = CodeVA + 124;
        value.x[0] = SharedVA;
        CHECK(candidate.run(value, 1) == NCE_DIRECT_BUDGET && value.pc == CodeVA + 128 && value.x[3] == 17);
        std::thread writer([&] {
            std::atomic_ref<uint64_t> shared(*ram->shared());
            shared.store(29, std::memory_order_seq_cst);
            shared.store(17, std::memory_order_seq_cst);
        });
        writer.join();
        value.x[3] = 31;
        CHECK(candidate.run(value) == NCE_DIRECT_UNSUPPORTED && value.pc == CodeVA + 132);
        CHECK(value.x[2] == (native ? 1U : 0U) && *ram->shared() == (native ? 17U : 31U));
    }
    report("native_exclusive_host_return_and_remote_aba");

    for (bool native : {false, true}) {
        Executor candidate(*ram, 0, native);
        // The guest stays inside one generated page while the writer performs ABA.
        const uint32_t words[] = {0xc85ffc03, 0xc89ffcc5, 0xc8dffd07, 0xb4ffffe7, 0xc802fc03, 0};
        ram->code(words, sizeof(words));
        *ram->shared() = 47;
        ram->shared()[32] = ram->shared()[64] = 0;
        auto value = state();
        value.x[0] = SharedVA;
        value.x[5] = 1;
        value.x[6] = SharedVA + 256;
        value.x[8] = SharedVA + 512;
        std::thread guest([&] {
            CHECK(candidate.run(value) == NCE_DIRECT_UNSUPPORTED && value.pc == CodeVA + 20);
        });
        std::atomic_ref<uint64_t> ready(ram->shared()[32]), resume(ram->shared()[64]), shared(*ram->shared());
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready.load(std::memory_order_acquire)) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        shared.store(53, std::memory_order_seq_cst);
        shared.store(47, std::memory_order_seq_cst);
        resume.store(1, std::memory_order_release);
        guest.join();
        CHECK(value.x[2] == (native ? 1U : 0U) && *ram->shared() == 47);
    }
    report("native_exclusive_remote_aba_without_host_return");

    for (bool mismatch : {false, true}) {
        const uint32_t words[] = {0xc85ffc03, mismatch ? 0x91002000U : 0xd5033f5fU, 0xc802fc03, 0};
        ram->code(words, sizeof(words));
        *ram->shared() = 41;
        ram->shared()[1] = 43;
        auto value = state();
        value.x[0] = SharedVA;
        CHECK(cpu[0]->run(value) == NCE_DIRECT_UNSUPPORTED && value.pc == CodeVA + 12);
        CHECK(value.x[2] == 1 && value.exclusive_address == UINT64_MAX);
        CHECK(*ram->shared() == 41 && ram->shared()[1] == 43);
    }
    report("native_exclusive_clrex_and_address_mismatch");

    std::array<uint32_t, 1029> collision{};
    collision[0] = 0xc85ffc03;  // ldaxr x3,[x0]
    collision[1] = 0x1403ffff;  // branch to the colliding route-table slot
    collision[1024] = 0x91000463;
    collision[1025] = 0xc802fc03;
    collision[1026] = 0x34000042;
    collision[1027] = 0xd61f00a0;
    ram->code(collision.data(), sizeof(collision));
    *ram->shared() = 59;
    auto value = state();
    value.x[0] = SharedVA;
    value.x[5] = CodeVA;
    CHECK(cpu[0]->run(value, 1000) == NCE_DIRECT_UNSUPPORTED && value.pc == CodeVA);
    CHECK(value.x[2] == 1 && value.x[3] == 60 && value.exclusive_address == UINT64_MAX);
    CHECK(*ram->shared() == 59);
    report("native_exclusive_interrupted_retry_fallback");
    {
        std::array<uint64_t, 10> guard{};
        guard[0] = guard[9] = 1;
        const uint32_t store[] = {0xf9000001, 0xd4200000};
        ram->code(store, sizeof(store));
        *ram->shared() = 17;
        for (auto &executor : cpu) {
            nce_direct_set_code_guard(executor->engine, guard.data(), guard.size());
            auto pending = state();
            pending.x[0] = SharedVA;
            pending.x[1] = 29;
            CHECK(executor->run(pending, 1000) == NCE_DIRECT_WRITE_FAULT);
            CHECK(pending.pc == CodeVA && *ram->shared() == 17);
        }
        // The owner has stopped both engines before releasing the code page.
        guard[9] = 0;
        auto pending = state();
        pending.x[0] = SharedVA;
        pending.x[1] = 29;
        CHECK(cpu[1]->run(pending, 1000) == NCE_DIRECT_UNSUPPORTED);
        CHECK(pending.pc == CodeVA + 4 && *ram->shared() == 29);
        nce_direct_set_code_guard(cpu[0]->engine, guard.data(), guard.size() - 1);
        pending = state();
        CHECK(cpu[0]->run(pending, 1000) == NCE_DIRECT_ERROR);
        for (auto &executor : cpu)
            nce_direct_set_code_guard(executor->engine, nullptr, 0);
    }
    report("shared_code_guard_across_executors");
    {
        std::array<uint64_t, 10> guard{};
        guard[0] = 1;
        std::array<std::unique_ptr<Executor>, 2> crossing;
        const uint32_t words[] = {0x3dc00000, 0x3d800020, 0};  // ldr q0,[x0]; str q0,[x1]
        ram->code(words, sizeof(words));
        for (unsigned i = 0; i < 2; ++i) {
            crossing[i] = std::make_unique<Executor>(*ram, i, true, true);
            nce_direct_set_code_guard(crossing[i]->engine, guard.data(), guard.size());
            for (unsigned byte = 0; byte < 16; ++byte)
                ram->bytes[(2 + i * 4) * PageBytes - 8 + byte] = i * 17 + byte;
        }
        parallel([&](unsigned i) {
            auto pending = state();
            pending.access_flags = NCE_DIRECT_UNALIGNED;
            pending.x[0] = DataVA + PageBytes - 8;
            pending.x[1] = DataVA + 3 * PageBytes - 8;
            CHECK(crossing[i]->run(pending) == NCE_DIRECT_UNSUPPORTED && pending.pc == CodeVA + 8);
            CHECK(!std::memcmp(ram->bytes.data() + (2 + i * 4) * PageBytes - 8,
                               ram->bytes.data() + (4 + i * 4) * PageBytes - 8, 16));
        });
        guard[8] = 1;
        std::memset(ram->bytes.data() + 8 * PageBytes - 8, 0, 16);
        auto pending = state();
        pending.access_flags = NCE_DIRECT_UNALIGNED;
        pending.x[0] = DataVA + PageBytes - 8;
        pending.x[1] = DataVA + 3 * PageBytes - 8;
        CHECK(crossing[1]->run(pending) == NCE_DIRECT_WRITE_FAULT && pending.pc == CodeVA + 4);
        for (unsigned byte = 0; byte < 16; ++byte)
            CHECK(!ram->bytes[8 * PageBytes - 8 + byte]);
    }
    report("parallel_cross_page_and_remote_code_guard");
}
