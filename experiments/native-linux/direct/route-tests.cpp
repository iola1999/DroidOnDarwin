/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"

namespace
{
struct Routes {
    alignas(4096) std::array<uint32_t, 1024> header{}, body{}, tail{};
    unsigned body_pages = 1;
    bool executable = true, return_to_start = false;
    NceDirect *engine = nce_direct_create({2 * 1024 * 1024, 256, 65536, false, false, false, true});
    Routes()
    {
        CHECK(engine);
    }
    ~Routes()
    {
        nce_direct_destroy(engine);
    }
    static const void *code(void *opaque, uint64_t address)
    {
        auto &self = *static_cast<Routes *>(opaque);
        if (address < CodeVA || (address & 4095))
            return nullptr;
        uint64_t index = (address - CodeVA) >> 12;
        if (!index)
            return self.header.data();
        if (index <= self.body_pages)
            return self.executable ? self.body.data() : nullptr;
        return index == self.body_pages + 1 && self.return_to_start ? self.tail.data() : nullptr;
    }
    NceDirectExit run(NceDirectState &state, uint64_t &fault)
    {
        NceDirectMemory memory{};
        memory.opaque = this;
        memory.map_page = [](void *, uint64_t, bool) -> void * { return nullptr; };
        memory.map_code = code;
        memory.mapping_epoch = 1;
        return nce_direct_run(engine, &state, memory, 2000000, &fault);
    }
};
}  // namespace

void direct_route_tests()
{
    auto owner = std::make_unique<Routes>();
    auto &test = *owner;
    test.header[0] = 0xd61f0020;  // br x1
    test.body[0] = 0x91000400;  // add x0,x0,#1
    test.body[1] = 0xd61f03c0;  // br x30
    NceDirectState initial{};
    for (unsigned index = 0; index < 31; ++index)
        initial.x[index] = 131 * index + 17;
    for (unsigned index = 0; index < 32; ++index) {
        initial.v[index][0] = 197 * index + 29;
        initial.v[index][1] = 241 * index + 31;
    }
    initial.pc = CodeVA;
    initial.x[1] = CodeVA + 4096;
    initial.x[30] = CodeVA + 4;
    initial.sp = DataVA;
    initial.nzcv = 0xb0000000;
    initial.exclusive_address = UINT64_MAX;
    uint64_t fault = 0;
    auto actual = initial, expected = initial;
    ++expected.x[0];
    expected.pc += 4;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));

    test.executable = false;
    actual = initial;
    expected = initial;
    expected.pc = CodeVA + 4096;
    CHECK(test.run(actual, fault) == NCE_DIRECT_FETCH_FAULT && fault == expected.pc);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    test.executable = true;
    test.body[0] = 0x91000c00;  // add x0,x0,#3
    actual = initial;
    expected = initial;
    expected.x[0] += 3;
    expected.pc += 4;
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    std::puts("{\"test\":\"route_epochs_fetch_permission_and_code_replacement\",\"passed\":true}");

    // The table retains the initial page's slot while a long branch chain
    // exhausts the code arena, then returns to that released cache entry.
    test.body_pages = 600;
    test.return_to_start = true;
    test.header.fill(0);
    test.header[0] = 0xb50000a1;  // cbnz x1,pc+20
    test.header[1] = 0xd2800021;  // mov x1,#1
    test.header[2] = 0x140003fe;  // b next page
    test.body.fill(0x91000400);  // add x0,x0,#1
    test.body[1023] = 0x14000001;
    test.tail[0] = 0x14000000 | (uint32_t(-int32_t((test.body_pages + 1) * 1024)) & 0x03ffffff);
    initial.x[1] = 0;
    actual = initial;
    expected = initial;
    expected.x[0] += test.body_pages * 1023;
    expected.x[1] = 1;
    expected.pc = CodeVA + 20;
    auto before = nce_direct_stats(test.engine);
    CHECK(test.run(actual, fault) == NCE_DIRECT_UNSUPPORTED);
    CHECK(!std::memcmp(&actual, &expected, sizeof(actual)));
    auto after = nce_direct_stats(test.engine);
    CHECK(after.flushes > before.flushes && after.route_invalidations > before.route_invalidations);
    CHECK(!after.route_cleared_entries && after.code_bytes <= after.code_capacity);
    std::puts("{\"test\":\"route_epochs_flush_and_return_to_freed_page\",\"passed\":true}");
}
