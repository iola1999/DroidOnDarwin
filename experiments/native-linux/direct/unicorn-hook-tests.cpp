/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <time.h>
extern "C" uc_err nce_uc_emu_stop_at_tb(uc_engine *);

namespace
{
struct Hooks {
    uint64_t calls = 0;
    bool stopping = false;
};
void written(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *opaque)
{
    auto &state = *static_cast<Hooks *>(opaque);
    uint64_t pc, counter;
    checked(uc_reg_read(uc, UC_ARM64_REG_PC, &pc));
    checked(uc_reg_read(uc, UC_ARM64_REG_X0, &counter));
    CHECK(type == UC_MEM_WRITE && address == DataVA && size == 8);
    CHECK(pc == CodeVA + 8 && counter == ++state.calls && counter == uint64_t(value));
    if (state.stopping && state.calls % 257 == 0)
        checked(nce_uc_emu_stop_at_tb(uc));
}

uc_engine *create(Hooks &state)
{
    uc_engine *uc;
    checked(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc));
    checked(uc_ctl_set_tcg_buffer_size(uc, 16 * 1024 * 1024));
    checked(uc_mem_map(uc, CodeVA, 4096, UC_PROT_ALL));
    checked(uc_mem_map(uc, DataVA, 4096, UC_PROT_ALL));
    // TBL, increment, store, compare, repeat, final sentinel.
    const uint32_t code[] = {0x4e010002, 0x91000400, 0xf9000020, 0xeb02001f, 0x54ffff81, 0xd4000001};
    checked(uc_mem_write(uc, CodeVA, code, sizeof(code)));
    uc_hook hook;
    checked(uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE, reinterpret_cast<void *>(written), &state, 1, 0));
    return uc;
}

void run(uc_engine *uc, Hooks &state, bool stopping)
{
    state = {0, stopping};
    uint64_t counter = 0, data = DataVA, iterations = 1000000, pc = CodeVA;
    checked(uc_reg_write(uc, UC_ARM64_REG_X0, &counter));
    checked(uc_reg_write(uc, UC_ARM64_REG_X1, &data));
    checked(uc_reg_write(uc, UC_ARM64_REG_X2, &iterations));
    struct timespec begin, end;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &begin));
    unsigned calls = 0;
    do {
        checked(uc_emu_start(uc, pc, CodeVA + 20, 0, 0));
        checked(uc_reg_read(uc, UC_ARM64_REG_PC, &pc));
        CHECK(++calls <= iterations);
    } while (pc != CodeVA + 20);
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &end));
    checked(uc_mem_read(uc, DataVA, &counter, sizeof(counter)));
    CHECK(counter == iterations && state.calls == iterations);
    CHECK(calls == (stopping ? 1 + iterations / 257 : 1));
    std::printf("{\"benchmark\":\"unicorn_write_hook_restore\",\"iterations\":%llu,\"stop_at_tb\":%s,\"wall_ns\":%llu}\n",
                iterations, stopping ? "true" : "false",
                uint64_t(end.tv_sec - begin.tv_sec) * 1000000000 + end.tv_nsec - begin.tv_nsec);
}
}  // namespace

void unicorn_hook_tests()
{
    Hooks first, second;
    uc_engine *a = create(first), *b = create(second);
    run(a, first, false);
    run(b, second, true);
    checked(uc_ctl_flush_tb(a));
    run(a, first, true);
    checked(uc_close(a));
    run(b, second, false);
    checked(uc_close(b));
    std::puts("{\"test\":\"unicorn_hook_state_cache_reset_and_independent_close\",\"passed\":true}");
}
