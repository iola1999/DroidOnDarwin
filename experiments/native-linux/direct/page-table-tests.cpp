/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include "page-table-layout.h"
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
    uint64_t epoch = 1;
    explicit Tables(unsigned mode) : engine(nce_direct_create({2 * 1024 * 1024, 256, 256, mode == 1, false})),
                                     table{root.data(), CodeVA, (UINT64_C(1) << 37) - CodeVA, PhysicalBase, ram.data(), ram.size()},
                                     refill_reads(mode == 1),
                                     flat(mode == 2 ? DIRECT_FLAT_BYTES / sizeof(nce_direct_flat_page) : 0)
    {
        CHECK(engine);
        if (!flat.empty())
            table.user_size = UINT64_C(1) << DIRECT_FLAT_USER_BITS;
        root[0] = uintptr_t(middle.data()) | ReadWrite;
        middle[0] = uintptr_t(leaves.data()) | ReadWrite;
        leaves[CodeVA >> 14] = PhysicalBase | DIRECT_PTE_READ_REQUIRED | 0x200;
        data_page(1);
    }
    ~Tables()
    {
        nce_direct_destroy(engine);
    }
    void data_page(unsigned index, uint64_t flags = ReadWrite)
    {
        leaves[DataVA >> 14] = PhysicalBase + index * PageBytes + flags;
        ++epoch;
    }
    void code(uint32_t instruction)
    {
        std::memset(ram.data(), 0, PageBytes);
        std::memcpy(ram.data(), &instruction, sizeof(instruction));
    }
    static void *map(void *opaque, uint64_t address, bool write)
    {
        auto &self = *static_cast<Tables *>(opaque);
        if (address < CodeVA || address >= DataVA + PageBytes)
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
        if (address < CodeVA || address >= CodeVA + PageBytes || !(self.leaves[address >> 14] & 0x200))
            return nullptr;
        return map(opaque, address, false);
    }
    NceDirectExit run(NceDirectState &state, uint64_t &fault)
    {
        if (!flat.empty()) {
            for (uint64_t address : {CodeVA, DataVA})
                flat[(address - CodeVA) >> 14] = {uintptr_t(map(this, address, false)), uintptr_t(map(this, address, true))};
        }
        NceDirectMemory memory{this, map, fetch, epoch, 14, &table, refill_reads, flat.empty() ? nullptr : flat.data(), nullptr};
        return nce_direct_run(engine, &state, memory, 1000, &fault);
    }
};
void report(const char *name, unsigned mode)
{
    std::printf("{\"test\":\"%s%s\",\"passed\":true}\n", name, mode == 2 ? "_flat" : mode == 1 ? "_refill"
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
    report("direct_pte_code_alias_and_next_run_release", mode);
}

void direct_page_table_tests()
{
    for (unsigned mode = 0; mode < 3; ++mode)
        check_page_tables(mode);

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
    NceDirectMemory memory{&test, Tables::map, Tables::fetch, test.epoch, 14, &test.table, false, second.data(), nullptr};
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
