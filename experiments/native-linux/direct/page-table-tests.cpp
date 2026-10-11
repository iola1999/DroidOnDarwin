/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include "page-table-layout.h"
#include "../translation/read-view.h"
#include <cerrno>
#include <csignal>
#include <random>

namespace
{
constexpr size_t PageBytes = 16384;
constexpr uint64_t PhysicalBase = UINT64_C(0x500000000);
constexpr uint64_t ReadWrite = DIRECT_PTE_WRITE_REQUIRED;
struct Tables {
    alignas(PageBytes) std::array<uint64_t, 2048> root{}, middle{}, leaves{};
    alignas(PageBytes) std::array<unsigned char, 4 * PageBytes> ram{};
    NceDirect *engine;
    NceDirectPageTable table;
    bool refill_reads;
    std::vector<nce_direct_flat_page> flat;
    uint64_t epoch = 1, code_epoch = 1;
    bool retain_routes = false;
    const void *untracked_code = nullptr;
    uint64_t code_limit = CodeVA + PageBytes;
    NceReadView read_view{};
    std::array<uintptr_t, 3> view_pages{};
    explicit Tables(unsigned mode, bool cross_page = false, bool retain = false, bool groups = false) : engine(nce_direct_create({2 * 1024 * 1024, 256, 256, mode == 1, false, cross_page, retain, retain, groups})),
                                                                                                        table{root.data(), CodeVA, (UINT64_C(1) << 37) - CodeVA, PhysicalBase, ram.data(), ram.size()},
                                                                                                        refill_reads(mode == 1),
                                                                                                        flat(mode >= 2 ? DIRECT_FLAT_BYTES / sizeof(nce_direct_flat_page) : 0)
    {
        CHECK(engine);
        retain_routes = retain;
        if (!flat.empty())
            table.user_size = UINT64_C(1) << DIRECT_FLAT_USER_BITS;
        if (mode == 3)
            CHECK(!nce_read_view_create(&read_view, table.user_size));
        root[0] = uintptr_t(middle.data()) | ReadWrite;
        middle[0] = uintptr_t(leaves.data()) | ReadWrite;
        leaves[CodeVA >> 14] = PhysicalBase | DIRECT_PTE_READ_REQUIRED | 0x200;
        data_page(1);
    }
    ~Tables()
    {
        CHECK(!nce_read_view_destroy(&read_view));
        nce_direct_destroy(engine);
    }
    void data_page(unsigned index, uint64_t flags = ReadWrite)
    {
        leaves[DataVA >> 14] = PhysicalBase + index * PageBytes + flags;
        ++epoch;
    }
    void code(uint32_t instruction)
    {
        ++code_epoch;
        std::memset(ram.data(), 0, PageBytes);
        std::memcpy(ram.data(), &instruction, sizeof(instruction));
    }
    static void *map(void *opaque, uint64_t address, bool write)
    {
        auto &self = *static_cast<Tables *>(opaque);
        if (address < CodeVA || address >= DataVA + 2 * PageBytes)
            return nullptr;
        uint64_t value = self.leaves[address >> 14];
        uint64_t required = write ? DIRECT_PTE_WRITE_REQUIRED : DIRECT_PTE_READ_REQUIRED;
        if ((value & required) != required || (value & DIRECT_PTE_PROTNONE))
            return nullptr;
        uint64_t offset = (value & ~UINT64_C(16383)) - PhysicalBase;
        return offset < self.ram.size() ? self.ram.data() + offset + (address & 16383) : nullptr;
    }
    static const void *fetch(void *opaque, uint64_t address)
    {
        auto &self = *static_cast<Tables *>(opaque);
        if (address < CodeVA || address >= self.code_limit || !(self.leaves[address >> 14] & 0x200))
            return nullptr;
        return map(opaque, address, false);
    }
    static uint64_t version(void *opaque, const void *address)
    {
        auto &self = *static_cast<Tables *>(opaque);
        return address == self.untracked_code ? 0 : self.code_epoch;
    }
    NceDirectExit run(NceDirectState &state, uint64_t &fault)
    {
        if (!flat.empty()) {
            unsigned index = 0;
            for (uint64_t address : {CodeVA, DataVA, DataVA + PageBytes}) {
                flat[(address - CodeVA) >> 14] = {uintptr_t(map(this, address, false)), uintptr_t(map(this, address, true))};
                uintptr_t read = flat[(address - CodeVA) >> 14].read;
                if (read_view.base && view_pages[index] != read) {
                    CHECK(!(read ? nce_read_view_map(&read_view, address - CodeVA, (void *)read, PageBytes)
                                 : nce_read_view_revoke(&read_view, address - CodeVA, PageBytes)));
                    view_pages[index] = read;
                }
                ++index;
            }
        }
        NceDirectMemory memory{this, map, fetch, epoch, 14, &table, refill_reads, flat.empty() ? nullptr : flat.data(),
                               retain_routes ? version : nullptr, retain_routes ? code_epoch : 0, read_view.base};
        return nce_direct_run(engine, &state, memory, 1000, &fault);
    }
};
void report(const char *name, unsigned mode)
{
    std::printf("{\"test\":\"%s%s\",\"passed\":true}\n", name, mode == 3 ? "_view" : mode == 2 ? "_flat"
                                                                                 : mode == 1   ? "_refill"
                                                                                               : "");
}
}  // namespace

static void check_page_tables(unsigned mode)
{
    auto holder = std::make_unique<Tables>(mode);
    auto &test = *holder;
    std::mt19937_64 random(0x4c4b4c);
    NceDirectState initial{};
    for (auto &reg : initial.x)
        reg = random();
    for (auto &vector : initial.v) {
        vector[0] = random();
        vector[1] = random();
    }
    initial.pc = CodeVA;
    initial.sp = DataVA + 512;
    initial.access_flags = NCE_DIRECT_UNALIGNED;
    initial.exclusive_address = UINT64_MAX;
    uint64_t fault = 0, value = 0x123456789abc;
    std::memcpy(test.ram.data() + PageBytes, &value, 8);
    test.code(0xf8408401);  // ldr x1,[x0],#8
    for (unsigned flags = 0; flags < 16; ++flags) {
        initial.nzcv = flags << 28;
        initial.x[0] = DataVA;
        auto actual = initial, expected = initial;
        expected.x[0] += 8;
        expected.x[1] = value;
        expected.pc += 4;
        CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
        CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    }
    report("direct_pte_registers_flags_and_post_index", mode);

    for (uint64_t flags : {UINT64_C(0), ReadWrite & ~DIRECT_PTE_PRESENT, ReadWrite & ~DIRECT_PTE_USER,
                           ReadWrite & ~DIRECT_PTE_YOUNG, ReadWrite | DIRECT_PTE_PROTNONE}) {
        test.data_page(1, flags);
        auto actual = initial;
        CHECK(test.run(actual, fault) == NCE_DIRECT_READ_FAULT && fault == DataVA);
        CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
    }
    test.data_page(1);
    for (uint64_t address : {UINT64_C(0), DataVA + (UINT64_C(1) << 25), DataVA + (UINT64_C(1) << 36), DataVA + (UINT64_C(1) << 48)}) {
        auto actual = initial;
        actual.x[0] = address;
        auto expected = actual;
        CHECK(test.run(actual, fault) == NCE_DIRECT_READ_FAULT && fault == address);
        CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    }
    for (uint64_t physical : {PhysicalBase - PageBytes, PhysicalBase + test.ram.size()}) {
        test.leaves[DataVA >> 14] = physical | ReadWrite;
        ++test.epoch;
        auto actual = initial;
        CHECK(test.run(actual, fault) == NCE_DIRECT_READ_FAULT && fault == DataVA);
        CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
    }
    report("direct_pte_read_permissions_directories_and_bounds", mode);

    test.code(0xf8008401);  // str x1,[x0],#8
    initial.x[1] = 17;
    for (uint64_t flags : {ReadWrite & ~DIRECT_PTE_WRITE, ReadWrite & ~DIRECT_PTE_DIRTY}) {
        test.data_page(1, flags);
        auto actual = initial;
        auto before = test.ram;
        CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT && fault == DataVA);
        CHECK(!std::memcmp(&actual, &initial, sizeof(actual)) && before == test.ram);
    }
    test.data_page(1);
    auto actual = initial;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED && actual.x[0] == DataVA + 8);
    std::memcpy(&value, test.ram.data() + PageBytes, 8);
    CHECK(value == 17);
    test.data_page(2);
    actual = initial;
    actual.x[1] = 29;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    std::memcpy(&value, test.ram.data() + 2 * PageBytes, 8);
    CHECK(value == 29);
    test.data_page(2, DIRECT_PTE_READ_REQUIRED);
    actual = initial;
    CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT && actual.pc == CodeVA);
    report("direct_pte_write_permissions_remap_and_retry", mode);

    test.data_page(0);
    initial.x[0] = DataVA + 128;
    actual = initial;
    auto before = test.ram;
    CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT && fault == DataVA + 128);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)) && before == test.ram);
    std::memcpy(test.ram.data() + 2 * PageBytes, test.ram.data(), PageBytes);
    test.leaves[CodeVA >> 14] = PhysicalBase + 2 * PageBytes + DIRECT_PTE_READ_REQUIRED + 0x200;
    ++test.epoch;
    actual = initial;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    std::memcpy(&value, test.ram.data() + 128, 8);
    CHECK(value == 17);
    CHECK((nce_direct_stats(test.engine).pte_read_refills != 0) == (mode == 1));
    auto stats = nce_direct_stats(test.engine);
    CHECK((stats.read_view_sites != 0) == (mode == 3));
    CHECK((stats.read_view_faults != 0) == (mode == 3));
    report("direct_pte_code_alias_and_next_run_release", mode);
}

static void check_cross_pages(unsigned mode)
{
    auto holder = std::make_unique<Tables>(mode, true);
    auto &test = *holder;
    auto second_page = [&](unsigned physical, uint64_t flags) {
        test.leaves[(DataVA + PageBytes) >> 14] = PhysicalBase + physical * PageBytes + flags;
        ++test.epoch;
    };
    second_page(3, ReadWrite);
    NceDirectState initial{};
    initial.pc = CodeVA;
    initial.x[0] = DataVA + PageBytes - 8;
    initial.x[1] = UINT64_C(0x123456789abcdef0);
    initial.x[2] = UINT64_C(0xfedcba9876543210);
    initial.nzcv = 0xa0000000;
    initial.access_flags = NCE_DIRECT_UNALIGNED;
    initial.exclusive_address = UINT64_MAX;
    uint64_t fault = 0;
    test.code(0xa8810801);  // stp x1,x2,[x0],#16
    auto actual = initial, expected = initial;
    expected.x[0] += 16;
    expected.pc += 4;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    CHECK(!std::memcmp(test.ram.data() + 2 * PageBytes - 8, &initial.x[1], 8));
    CHECK(!std::memcmp(test.ram.data() + 3 * PageBytes, &initial.x[2], 8));
    test.code(0xa8c10801);  // ldp x1,x2,[x0],#16
    actual = initial;
    actual.x[1] = actual.x[2] = 0;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));

    for (uint64_t flags : {ReadWrite & ~DIRECT_PTE_PRESENT, ReadWrite & ~DIRECT_PTE_USER, ReadWrite | DIRECT_PTE_PROTNONE}) {
        second_page(3, flags);
        actual = initial;
        auto before = test.ram;
        CHECK(test.run(actual, fault) == NCE_DIRECT_READ_FAULT && fault == DataVA + PageBytes);
        CHECK(!std::memcmp(&actual, &initial, sizeof(actual)) && before == test.ram);
    }
    test.code(0xa8810801);
    for (uint64_t flags : {ReadWrite & ~DIRECT_PTE_WRITE, ReadWrite & ~DIRECT_PTE_DIRTY}) {
        second_page(3, flags);
        actual = initial;
        actual.x[1] = 71;
        actual.x[2] = 93;
        auto before = test.ram;
        auto before_state = actual;
        CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT && fault == DataVA + PageBytes);
        CHECK(!std::memcmp(&actual, &before_state, sizeof(actual)) && before == test.ram);
        second_page(3, ReadWrite);
        auto resumed = actual;
        resumed.pc += 4;
        resumed.x[0] += 16;
        CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
        CHECK(!std::memcmp(&actual, &resumed, sizeof(actual)));
        CHECK(!std::memcmp(test.ram.data() + 2 * PageBytes - 8, &before_state.x[1], 8));
        CHECK(!std::memcmp(test.ram.data() + 3 * PageBytes, &before_state.x[2], 8));
    }

    second_page(0, ReadWrite);
    actual = initial;
    auto before = test.ram;
    CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT && fault == DataVA + PageBytes);
    CHECK(!std::memcmp(&actual, &initial, sizeof(actual)) && before == test.ram);
    std::memcpy(test.ram.data() + 2 * PageBytes, test.ram.data(), PageBytes);
    test.leaves[CodeVA >> 14] = PhysicalBase + 2 * PageBytes + DIRECT_PTE_READ_REQUIRED + 0x200;
    ++test.epoch;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    CHECK(!std::memcmp(test.ram.data(), &initial.x[2], 8));
    auto stats = nce_direct_stats(test.engine);
    CHECK(stats.cross_page_completed == 5 && stats.cross_page_faults == 6 && !stats.cross_page_fallbacks);
    report("direct_pte_cross_page_permissions_retry_and_code_alias", mode);
}

static void check_retained_routes(unsigned mode)
{
    auto holder = std::make_unique<Tables>(mode, true, true);
    auto &test = *holder;
    const uint64_t target = CodeVA + PageBytes;
    test.code_limit = target + PageBytes;
    test.leaves[target >> 14] = PhysicalBase + 2 * PageBytes + DIRECT_PTE_READ_REQUIRED + 0x200;
    test.code(0xd61f0020);  // br x1
    uint32_t *body = reinterpret_cast<uint32_t *>(test.ram.data() + 2 * PageBytes);
    body[0] = 0x91000400;  // add x0,x0,#1
    body[1] = 0xd65f03c0;  // ret
    NceDirectState initial{};
    std::mt19937_64 random(0x524f555445);
    for (auto &reg : initial.x)
        reg = random();
    for (auto &vector : initial.v) {
        vector[0] = random();
        vector[1] = random();
    }
    initial.pc = CodeVA;
    initial.x[1] = target;
    initial.x[30] = CodeVA + 4;
    initial.nzcv = 0xb0000000;
    initial.access_flags = NCE_DIRECT_UNALIGNED;
    initial.exclusive_address = UINT64_MAX;
    uint64_t fault = 0;
    auto completed = [&](uint64_t increment) {
        auto actual = initial, expected = initial;
        expected.x[0] += increment;
        expected.pc += 4;
        CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
        CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    };
    completed(1);
    auto before = nce_direct_stats(test.engine);
    completed(1);
    auto after = nce_direct_stats(test.engine);
    CHECK(after.route_reused_runs == before.route_reused_runs + 1);
    CHECK(after.branch_exits == before.branch_exits);
    report("retained_routes_registers_and_cached_target", mode);

    test.leaves[target >> 14] &= ~UINT64_C(0x200);
    ++test.epoch;
    auto actual = initial, expected = initial;
    expected.pc = target;
    CHECK(test.run(actual, fault) == NCE_DIRECT_FETCH_FAULT && fault == target);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    test.leaves[target >> 14] |= 0x200;
    ++test.epoch;
    completed(1);
    body[0] = 0x91000c00;  // add x0,x0,#3
    ++test.code_epoch;
    completed(3);
    std::memcpy(test.ram.data() + 3 * PageBytes, body, 4096);
    body = reinterpret_cast<uint32_t *>(test.ram.data() + 3 * PageBytes);
    body[0] = 0x91001c00;  // add x0,x0,#7
    test.leaves[target >> 14] = PhysicalBase + 3 * PageBytes + DIRECT_PTE_READ_REQUIRED + 0x200;
    ++test.epoch;
    completed(7);
    before = nce_direct_stats(test.engine);
    initial.access_flags = 0;
    completed(7);
    after = nce_direct_stats(test.engine);
    CHECK(after.compiles > before.compiles && after.route_reused_runs == before.route_reused_runs);
    report("retained_routes_permissions_code_remap_and_flags", mode);

    test.untracked_code = body;
    ++test.code_epoch;
    completed(7);
    before = nce_direct_stats(test.engine);
    body[0] = 0x91001400;  // add x0,x0,#5; this alias has no write notification
    completed(5);
    after = nce_direct_stats(test.engine);
    CHECK(after.route_reused_runs == before.route_reused_runs && after.code_changes > before.code_changes);
    test.untracked_code = nullptr;
    test.code_epoch = 0;
    completed(5);
    before = nce_direct_stats(test.engine);
    completed(5);
    CHECK(nce_direct_stats(test.engine).route_reused_runs == before.route_reused_runs);
    report("retained_routes_untracked_alias_and_zero_epoch", mode);

    // The second run reaches the old target through the cached branch. Its
    // physical page must remain guarded even though resolve() is bypassed.
    test.code_epoch = 100;
    test.data_page(3);
    body[0] = 0xb4000042;  // cbz x2,pc+8
    body[1] = 0xf9000083;  // str x3,[x4]
    body[2] = 0xd65f03c0;
    initial.x[2] = 0;
    initial.x[4] = DataVA + 128;
    completed(0);
    before = nce_direct_stats(test.engine);
    initial.x[2] = 1;
    actual = initial;
    expected = initial;
    expected.pc = target + 4;
    auto bytes = test.ram;
    CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT && fault == initial.x[4]);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)) && bytes == test.ram);
    after = nce_direct_stats(test.engine);
    CHECK(after.route_reused_runs == before.route_reused_runs + 1 && after.branch_exits == before.branch_exits);
    report("retained_routes_cached_target_write_alias", mode);
}

static void check_memory_groups(unsigned mode)
{
    auto holder = std::make_unique<Tables>(mode, true, false, true);
    auto &test = *holder;
    std::mt19937_64 random(0x475250505445);
    NceDirectState initial{};
    initial.pc = CodeVA;
    initial.access_flags = NCE_DIRECT_UNALIGNED;
    initial.exclusive_address = UINT64_MAX;
    for (auto &reg : initial.x)
        reg = random();
    for (auto &byte : test.ram)
        byte = random();
    uint64_t fault = 0;
    for (bool write : {false, true}) {
        std::array<uint32_t, 5> code{};
        for (unsigned i = 0; i < 4; ++i)
            code[i] = (write ? 0xf9000000 : 0xf9400000) | (i << 10) | (i + 1);
        test.code(0);
        std::memcpy(test.ram.data(), code.data(), sizeof(code));
        for (unsigned physical : {1, 3}) {
            test.data_page(physical);
            for (unsigned completed = 1; completed <= 4; ++completed) {
                initial.x[0] = DataVA + PageBytes - completed * 8;
                auto actual = initial, expected = initial;
                auto expected_ram = test.ram;
                for (unsigned i = 0; i < completed; ++i) {
                    auto pointer = expected_ram.data() + (physical + 1) * PageBytes - (completed - i) * 8;
                    if (write)
                        std::memcpy(pointer, &expected.x[i + 1], 8);
                    else
                        std::memcpy(&expected.x[i + 1], pointer, 8);
                }
                expected.pc += completed * 4;
                CHECK(test.run(actual, fault) == (completed == 4 ? NCE_DIRECT_UNSUPPORTED : write ? NCE_DIRECT_WRITE_FAULT
                                                                                                  : NCE_DIRECT_READ_FAULT));
                if (completed != 4)
                    CHECK(fault == DataVA + PageBytes);
                CHECK(!std::memcmp(&actual, &expected, sizeof(actual)) && expected_ram == test.ram);
            }
        }
        test.data_page(1, 0);
        auto actual = initial;
        CHECK(test.run(actual, fault) == (write ? NCE_DIRECT_WRITE_FAULT : NCE_DIRECT_READ_FAULT));
        CHECK(fault == initial.x[0] && !std::memcmp(&actual, &initial, sizeof(actual)));
        if (write) {
            test.data_page(0);
            actual = initial;
            auto before = test.ram;
            CHECK(test.run(actual, fault) == NCE_DIRECT_WRITE_FAULT && fault == initial.x[0]);
            CHECK(!std::memcmp(&actual, &initial, sizeof(actual)) && before == test.ram);
        }
    }
    auto stats = nce_direct_stats(test.engine);
    CHECK(stats.memory_groups >= 2 && stats.memory_group_instructions >= 8);
    report("direct_pte_memory_groups_boundaries_remap_and_code_alias", mode);
}

static volatile sig_atomic_t forwarded_signal;
static void observe_signal(int number)
{
    forwarded_signal = number;
}

static void check_read_view_handlers()
{
    auto holder = std::make_unique<Tables>(3);
    holder->code(0xf9400001);
    NceDirectState state{};
    state.pc = CodeVA;
    state.x[0] = DataVA;
    auto before = state;
    uint64_t fault = 0;
    CHECK(holder->run(state, fault) == NCE_DIRECT_ERROR);
    CHECK(!std::memcmp(&state, &before, sizeof(state)));

    struct sigaction observer{}, original[2], installed[2], current{};
    observer.sa_handler = observe_signal;
    sigemptyset(&observer.sa_mask);
    constexpr int signals[] = {SIGBUS, SIGSEGV};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!sigaction(signals[i], &observer, &original[i]));
    CHECK(!nce_direct_install_read_faults());
    CHECK(nce_direct_install_read_faults() == -1 && errno == EBUSY);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!sigaction(signals[i], nullptr, &installed[i]));
        forwarded_signal = 0;
        CHECK(!raise(signals[i]) && forwarded_signal == signals[i]);
    }
    CHECK(!sigaction(SIGSEGV, &observer, nullptr));
    CHECK(nce_direct_remove_read_faults() == -1 && errno == EBUSY);
    CHECK(!sigaction(SIGBUS, nullptr, &current));
    CHECK(current.sa_sigaction == installed[0].sa_sigaction);
    CHECK(!sigaction(SIGSEGV, &installed[1], nullptr));
    CHECK(!nce_direct_remove_read_faults());
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(!sigaction(signals[i], nullptr, &current));
        CHECK(current.sa_handler == observer.sa_handler);
        CHECK(!sigaction(signals[i], &original[i], nullptr));
    }
    report("direct_read_view_handler_ownership_and_forwarding", 0);
}

static void check_read_view_vectors()
{
    auto flat = std::make_unique<Tables>(2), view = std::make_unique<Tables>(3);
    std::mt19937_64 random(0x76696577);
    NceDirectState initial{};
    for (auto &reg : initial.x)
        reg = random();
    for (auto &vector : initial.v)
        for (auto &part : vector)
            part = random();
    initial.pc = CodeVA;
    initial.x[18] = DataVA;
    initial.sp = DataVA + 512;
    initial.nzcv = 0xa0000000;
    initial.access_flags = NCE_DIRECT_UNALIGNED;
    initial.exclusive_address = UINT64_MAX;
    for (unsigned i = 0; i < 128; ++i)
        flat->ram[PageBytes + i] = view->ram[PageBytes + i] = random();
    const uint32_t words[] = {
        0x3cc10400 | (18 << 5) | 31,  // ldr q31,[x18],#16
        0xacc10000 | (18 << 5) | 31,  // ldp q31,q0,[x18],#32
        0x4cdf7000 | (18 << 5) | 31,  // ld1 {v31.16b},[x18],#16
        0x0ddf8000 | (18 << 5) | 31,  // ld1 {v31.s}[0],[x18],#4
        0x9c000000 | ((DataVA - CodeVA) / 4 << 5) | 31  // ldr q31,literal
    };
    for (auto word : words) {
        flat->code(word);
        view->code(word);
        auto expected = initial, actual = initial;
        uint64_t fault = 0;
        CHECK(flat->run(expected, fault) == NCE_DIRECT_UNSUPPORTED && expected.pc == CodeVA + 4);
        view->data_page(1, 0);
        auto faults = nce_direct_stats(view->engine).read_view_faults;
        CHECK(view->run(actual, fault) == NCE_DIRECT_READ_FAULT && fault == DataVA);
        CHECK(!std::memcmp(&actual, &initial, sizeof(actual)));
        CHECK(nce_direct_stats(view->engine).read_view_faults == faults + 1);
        view->data_page(1);
        CHECK(view->run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
        CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    }
    report("direct_read_view_vector_literal_fault_and_retry", 0);
}

void direct_page_table_tests()
{
    check_read_view_handlers();
    CHECK(!nce_direct_install_read_faults());
    for (unsigned mode = 0; mode < 4; ++mode) {
        check_page_tables(mode);
        check_cross_pages(mode);
        check_memory_groups(mode);
        if (mode != 1)
            check_retained_routes(mode);
    }
    check_read_view_vectors();
    CHECK(!nce_direct_remove_read_faults());

    auto holder = std::make_unique<Tables>(2);
    auto &test = *holder;
    NceDirectState initial{};
    initial.pc = CodeVA;
    initial.x[0] = DataVA;
    initial.nzcv = 0xf0000000;
    initial.access_flags = NCE_DIRECT_UNALIGNED;
    initial.exclusive_address = UINT64_MAX;
    uint64_t fault = 0, value;
    for (unsigned width = 0; width < 4; ++width) {
        const uint32_t words[] = {(width << 30) | 0x085ffc01, 0x91000421, (width << 30) | 0x0802fc01, 0};
        std::memcpy(test.ram.data(), words, sizeof(words));
        value = 17;
        std::memcpy(test.ram.data() + PageBytes, &value, 8);
        auto actual = initial;
        CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED && actual.pc == CodeVA + 12);
        CHECK(actual.x[1] == 18 && actual.x[2] == 0 && actual.nzcv == initial.nzcv);
        std::memcpy(&value, test.ram.data() + PageBytes, 8);
        CHECK(value == 18);
    }
    const uint32_t pair[] = {0xa9000801, 0xa9401003, 0};  // stp x1,x2,[x0]; ldp x3,x4,[x0]
    std::memcpy(test.ram.data(), pair, sizeof(pair));
    auto actual = initial;
    actual.x[1] = 23;
    actual.x[2] = 51;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED && actual.pc == CodeVA + 8);
    CHECK(actual.x[3] == 23 && actual.x[4] == 51 && actual.nzcv == initial.nzcv);
    test.code(0xd50b7420);  // dc zva,x0
    actual = initial;
    actual.access_flags |= NCE_DIRECT_DC_ZVA64;
    std::memset(test.ram.data() + PageBytes, 0xff, 128);
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED && actual.pc == CodeVA + 4);
    CHECK(actual.nzcv == initial.nzcv);
    for (unsigned i = 0; i < 128; ++i)
        CHECK(test.ram[PageBytes + i] == (i < 64 ? 0 : 0xff));
    report("direct_flat_exclusive_pair_and_zva", 0);

    test.code(0xf9400001);  // ldr x1,[x0]
    actual = initial;
    actual.x[0] = DataVA + PageBytes - 4;
    auto before = actual;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &before, sizeof(actual)));
    auto second = test.flat;
    second[(DataVA - CodeVA) >> 14].read = uintptr_t(test.ram.data() + 2 * PageBytes);
    value = 97;
    std::memcpy(test.ram.data() + 2 * PageBytes, &value, 8);
    NceDirectMemory memory{&test, Tables::map, Tables::fetch, test.epoch, 14, &test.table, false, second.data(), nullptr, 0, nullptr};
    actual = initial;
    CHECK(nce_direct_run(test.engine, &actual, memory, 1000, &fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(actual.x[1] == 97);
    memory.refill_reads = true;
    actual = initial;
    CHECK(nce_direct_run(test.engine, &actual, memory, 1000, &fault) == NCE_DIRECT_ERROR);
    memory.refill_reads = false;
    test.table.user_size -= PageBytes;
    CHECK(nce_direct_run(test.engine, &actual, memory, 1000, &fault) == NCE_DIRECT_ERROR);
    report("direct_flat_cross_page_table_switch_and_configuration", 0);
}
