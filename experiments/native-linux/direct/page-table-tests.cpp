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
    uint64_t epoch = 1;
    explicit Tables(bool refill) : engine(nce_direct_create({2 * 1024 * 1024, 256, 256, refill, false})),
                                   table{root.data(), CodeVA, (UINT64_C(1) << 37) - CodeVA, PhysicalBase, ram.data(), ram.size()},
                                   refill_reads(refill)
    {
        CHECK(engine);
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
        NceDirectMemory memory{this, map, fetch, epoch, 14, &table, refill_reads};
        return nce_direct_run(engine, &state, memory, 1000, &fault);
    }
};
void report(const char *name, bool refill)
{
    std::printf("{\"test\":\"%s%s\",\"passed\":true}\n", name, refill ? "_refill" : "");
}
}  // namespace

static void check_page_tables(bool refill)
{
    auto holder = std::make_unique<Tables>(refill);
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
    report("direct_pte_registers_flags_and_post_index", refill);

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
    report("direct_pte_read_permissions_directories_and_bounds", refill);

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
    report("direct_pte_write_permissions_remap_and_retry", refill);

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
    CHECK((nce_direct_stats(test.engine).pte_read_refills != 0) == refill);
    report("direct_pte_code_alias_and_next_run_release", refill);
}

void direct_page_table_tests()
{
    check_page_tables(false);
    check_page_tables(true);
}
