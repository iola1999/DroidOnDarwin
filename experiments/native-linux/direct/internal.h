/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "direct.h"
#include "layout.h"
#include <array>
#include <vector>
namespace nce_direct
{
constexpr uint32_t CompileExclusive = 512;
constexpr uint32_t CompileAes = 16384, CompileSha1 = 32768, CompileSha256 = 65536;
constexpr uint32_t CompilePauthNop = 131072;
constexpr uint32_t CompileData16K = 262144, CompileZva64 = 524288;
constexpr uint32_t CompilePageTable = 1048576;
constexpr uint32_t CompilePteReadRefill = 2097152;
constexpr uint32_t CompileFlat = 4194304;
enum Stop : uint64_t { Budget = 0,
                       Fallback = 1,
                       ReadPage = 2,
                       WritePage = 3,
                       CodeBranch = 5 };
struct Page {
    static constexpr size_t max_bytes = 1024 * 1024;
    std::array<uint32_t, 1024> source{};
    std::array<void *, 1024> entries{}, branch_entries{};
    uint32_t flags = 0;
    uint64_t validated_run = 0;
    size_t compile(uint64_t, const void *, size_t, uint32_t *, uint32_t, void **, unsigned);
};
struct Frame {
    NceDirectState state;
    uint64_t budget, address, reason;
    void *code_routes;
    uint64_t code_page;
    Page *code;
    void (*exit_stub)(), (*branch_stub)();
    const void *stop_request;
    uint64_t route_mask;
    NceDirectPageTable page_table;
    uint64_t *code_page_runs;
    uint64_t run_id;
    void (*read_pte_stub)(), (*write_pte_stub)();
    uint64_t pte_saved_nzcv, pte_saved_link, pte_required;
    uint64_t pte_read_refills;
    uint64_t pte_refill_mask;
};
struct Mapping {
    uint64_t read_tag, read_addend, write_tag, write_addend;
};
constexpr Mapping EmptyMapping{UINT64_MAX, 0, UINT64_MAX, 0};
struct CodeRoute {
    uint64_t tag;
    void **entries;
    Page *code;
    uint64_t reserved;
};
static_assert(sizeof(NceDirectState) == 816);
static_assert(sizeof(Mapping) == 32 && offsetof(Mapping, write_tag) == 16);
static_assert(sizeof(nce_direct_flat_page) == 16 && offsetof(nce_direct_flat_page, write) == 8);
static_assert(sizeof(CodeRoute) == 32 && offsetof(CodeRoute, code) == 16);
#define CHECK_OFFSET(field, macro) static_assert(offsetof(Frame, field) == macro)
CHECK_OFFSET(state.pc, DIRECT_PC);
CHECK_OFFSET(state.nzcv, DIRECT_NZCV);
CHECK_OFFSET(state.fpcr, DIRECT_FPCR);
CHECK_OFFSET(state.fpsr, DIRECT_FPSR);
CHECK_OFFSET(state.access_flags, DIRECT_ACCESS_FLAGS);
CHECK_OFFSET(state.tpidr_el0, DIRECT_TPIDR);
CHECK_OFFSET(state.v, DIRECT_VECTORS);
CHECK_OFFSET(state.exclusive_address, DIRECT_EXCLUSIVE_ADDR);
CHECK_OFFSET(state.exclusive_value, DIRECT_EXCLUSIVE_VALUE);
CHECK_OFFSET(budget, DIRECT_BUDGET);
CHECK_OFFSET(address, DIRECT_ADDRESS);
CHECK_OFFSET(reason, DIRECT_REASON);
CHECK_OFFSET(code_routes, DIRECT_CODE_ROUTES);
CHECK_OFFSET(code_page, DIRECT_CODE_PAGE);
CHECK_OFFSET(code, DIRECT_CODE);
CHECK_OFFSET(exit_stub, DIRECT_EXIT_STUB);
CHECK_OFFSET(branch_stub, DIRECT_BRANCH_STUB);
CHECK_OFFSET(stop_request, DIRECT_STOP_REQUEST);
CHECK_OFFSET(route_mask, DIRECT_ROUTE_MASK);
CHECK_OFFSET(page_table.root, DIRECT_PTE_ROOT);
CHECK_OFFSET(page_table.user_base, DIRECT_PTE_USER_BASE);
CHECK_OFFSET(page_table.user_size, DIRECT_PTE_USER_SIZE);
CHECK_OFFSET(page_table.physical_base, DIRECT_PTE_PHYSICAL_BASE);
CHECK_OFFSET(page_table.ram, DIRECT_PTE_RAM);
CHECK_OFFSET(page_table.ram_size, DIRECT_PTE_RAM_SIZE);
CHECK_OFFSET(code_page_runs, DIRECT_PTE_CODE_RUNS);
CHECK_OFFSET(run_id, DIRECT_RUN_ID);
CHECK_OFFSET(read_pte_stub, DIRECT_PTE_READ_STUB);
CHECK_OFFSET(write_pte_stub, DIRECT_PTE_WRITE_STUB);
CHECK_OFFSET(pte_saved_nzcv, DIRECT_PTE_SAVED_NZCV);
CHECK_OFFSET(pte_saved_link, DIRECT_PTE_SAVED_LINK);
CHECK_OFFSET(pte_required, DIRECT_PTE_REQUIRED);
CHECK_OFFSET(pte_read_refills, DIRECT_PTE_READ_REFILLS);
CHECK_OFFSET(pte_refill_mask, DIRECT_PTE_REFILL_MASK);
#undef CHECK_OFFSET
extern "C" void nce_direct_enter(Frame *, void *, const void *);
extern "C" void nce_direct_leave();
extern "C" void nce_direct_exit_stub();
extern "C" void nce_direct_branch_stub();
extern "C" void nce_direct_read_pte();
extern "C" void nce_direct_write_pte();
}  // namespace nce_direct
