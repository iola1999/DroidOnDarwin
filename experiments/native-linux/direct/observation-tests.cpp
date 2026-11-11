/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"

void direct_observation_tests()
{
    auto owner = std::make_unique<Probe>("observation");
    auto &probe = *owner;
    probe.config = {2 * 1024 * 1024, 256, 256, false, false, false, false, false, false, false, false, false};
    probe.direct = nce_direct_create(probe.config);
    CHECK(probe.direct);
    uint64_t completed = 0, fault = 0;
    auto memory = probe.memory();
    memory.completed_integer_pc = &completed;
    auto initial = [] {
        NceDirectState state{};
        state.pc = CodeVA;
        state.exclusive_address = UINT64_MAX;
        state.x[0] = 0x7fffffffffffffff;
        return state;
    };
    auto run = [&](NceDirectState &state, uint64_t budget = 1000) {
        return nce_direct_run(probe.direct, &state, memory, budget, &fault);
    };
    probe.code[0] = 0xb1000400;  // adds x0,x0,#1
    auto state = initial();
    CHECK(run(state) == NCE_DIRECT_UNSUPPORTED);
    CHECK(state.x[0] == 0x8000000000000000 && state.nzcv == 0x90000000);
    CHECK(completed == CodeVA && state.pc == CodeVA + 4);
    state = initial();
    CHECK(run(state, 1) == NCE_DIRECT_BUDGET && completed == UINT64_MAX);
    state = initial();
    nce_direct_prepare_run(probe.direct);
    nce_direct_request_stop(probe.direct);
    CHECK(run(state) == NCE_DIRECT_BUDGET && completed == UINT64_MAX);
    state = initial();
    CHECK(run(state, 0) == NCE_DIRECT_ERROR && completed == UINT64_MAX);

    probe.code[0] = 0;
    state = initial();
    CHECK(run(state) == NCE_DIRECT_UNSUPPORTED && completed == UINT64_MAX);
    probe.code[0] = 0xf9400020;  // ldr x0,[x1]
    state = initial();
    state.x[1] = DataVA + DataBytes;
    CHECK(run(state) == NCE_DIRECT_READ_FAULT && completed == UINT64_MAX);
    CHECK(state.pc == CodeVA && fault == state.x[1]);
    probe.code[0] = 0x91000400;  // add x0,x0,#1
    probe.code[1] = 0xf9400022;  // ldr x2,[x1]
    state = initial();
    state.x[1] = DataVA + DataBytes;
    CHECK(run(state) == NCE_DIRECT_READ_FAULT && completed == CodeVA);
    CHECK(state.pc == CodeVA + 4 && state.x[0] == 0x8000000000000000);
    std::puts("{\"test\":\"completed_integer_fault_stop_budget_and_flags\",\"passed\":true}");

    probe.code.fill(0);
    probe.code[0] = 0xd61f0020;  // br x1
    probe.code[1024] = 0x91000400;
    probe.code[1025] = 0xd61f03c0;  // br x30
    probe.code[2048] = 0x91000400;  // executable mapping left uncalled
    auto linked = initial();
    linked.x[1] = CodeVA + 4096;
    linked.x[30] = CodeVA + 4;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        state = linked;
        CHECK(run(state) == NCE_DIRECT_UNSUPPORTED);
        CHECK(completed == CodeVA + 4096 && state.pc == CodeVA + 4);
    }
    probe.extra_code_va = 0x100000000;
    state = linked;
    state.x[1] = probe.extra_code_va;
    CHECK(run(state) == NCE_DIRECT_UNSUPPORTED && completed == probe.extra_code_va);
    CHECK(state.pc == CodeVA + 4);
    memory.completed_integer_pc = nullptr;
    completed = 123;
    state = linked;
    CHECK(run(state) == NCE_DIRECT_UNSUPPORTED && completed == 123);
    memory.completed_integer_pc = &completed;
    state = linked;
    CHECK(run(state) == NCE_DIRECT_UNSUPPORTED && completed == CodeVA + 4096);
    probe.executable = false;
    state = linked;
    CHECK(run(state) == NCE_DIRECT_FETCH_FAULT && completed == UINT64_MAX);
    std::puts("{\"test\":\"completed_integer_link_alias_and_unexecuted_mapping\",\"passed\":true}");
}
