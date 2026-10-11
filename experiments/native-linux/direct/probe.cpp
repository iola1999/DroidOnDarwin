/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
static unsigned setting(const char *name, unsigned fallback)
{
    const char *text = std::getenv(name);
    if (!text)
        return fallback;
    char *end;
    unsigned long value = std::strtoul(text, &end, 10);
    CHECK(*text >= '0' && *text <= '9' && !*end && value <= UINT32_MAX);
    return unsigned(value);
}
Probe::Probe(const char *mode)
{
    if (!std::strcmp(mode, "direct") || !std::strcmp(mode, "tests")) {
        unsigned selective = setting("NCE_DIRECT_SELECTIVE_WRITES", 0);
        unsigned reuse = setting("NCE_DIRECT_REUSE_CODE_CHECKS", 0);
        unsigned epochs = setting("NCE_DIRECT_ROUTE_EPOCHS", 0);
        unsigned retain = setting("NCE_DIRECT_RETAIN_ROUTES", 0);
        unsigned groups = setting("NCE_DIRECT_MEMORY_GROUPS", 0);
        CHECK(selective <= 1 && reuse <= 1 && epochs <= 1 && retain <= 1 && groups <= 1);
        config = {16 * 1024 * 1024, setting("NCE_DIRECT_MAPPING_ENTRIES", 256),
                  setting("NCE_DIRECT_ROUTE_ENTRIES", 256), bool(selective), bool(reuse), false, bool(epochs), bool(retain), bool(groups), false};
        direct = nce_direct_create(config);
        CHECK(direct);
        std::printf("{\"direct_config\":true,\"mapping_entries\":%u,\"route_entries\":%u,\"selective_writes\":%s,\"reuse_code_checks\":%s,\"route_epochs\":%s,\"retain_routes\":%s,\"memory_groups\":%s}\n",
                    config.mapping_entries, config.route_entries, config.selective_writes ? "true" : "false", config.reuse_code_checks ? "true" : "false", config.route_epochs ? "true" : "false", config.retain_routes ? "true" : "false", config.memory_groups ? "true" : "false");
    }
    if (std::strcmp(mode, "tests") && std::strcmp(mode, "unicorn"))
        return;
    checked(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc));
    checked(uc_ctl_set_cpu_model(uc, UC_CPU_ARM64_MAX));
    checked(uc_ctl_set_page_size(uc, 16384));
    checked(uc_ctl_set_tcg_buffer_size(uc, 16 * 1024 * 1024));
    checked(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    checked(uc_mem_map_ptr(uc, 0x10000000, sizeof(code), UC_PROT_ALL, code.data()));
    checked(uc_mem_map_ptr(uc, 0x20000000, sizeof(reference), UC_PROT_ALL, reference.data()));
    uc_hook hook;
    checked(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL, reinterpret_cast<void *>(Fill), this, 1, 0));
}
Probe::~Probe()
{
    nce_direct_destroy(direct);
    if (uc)
        checked(uc_close(uc));
}
void *Probe::Map(void *opaque, uint64_t address, bool write)
{
    auto &probe = *static_cast<Probe *>(opaque);
    if (!write && probe.readable && address >= CodeVA && address < CodeVA + sizeof(probe.code))
        return (char *)probe.code.data() + address - CodeVA;
    if (address < DataVA || address >= DataVA + DataBytes || (write ? !probe.writable : !probe.readable))
        return nullptr;
    return probe.alias_code ? (char *)probe.code.data() + probe.alias_offset : (char *)probe.data.data() + address - DataVA;
}
const void *Probe::Code(void *opaque, uint64_t address)
{
    auto &probe = *static_cast<Probe *>(opaque);
    if (!probe.executable)
        return nullptr;
    if (probe.extra_code_va && address == probe.extra_code_va)
        return (char *)probe.code.data() + 4096;
    return address >= CodeVA && address < CodeVA + sizeof(probe.code) ? (char *)probe.code.data() + address - CodeVA : nullptr;
}
bool Probe::Fill(uc_engine *, uint64_t address, uc_mem_type, uc_tlb_entry *entry, void *opaque)
{
    auto &probe = *static_cast<Probe *>(opaque);
    ++probe.fills;
    uint64_t page = address & ~UINT64_C(16383);
    if (page >= CodeVA && page < CodeVA + sizeof(probe.code)) {
        entry->paddr = 0x10000000 + page - CodeVA;
        entry->perms = uc_prot(UC_PROT_READ | UC_PROT_EXEC);
        return true;
    }
    if (page >= DataVA && page < DataVA + DataBytes) {
        entry->paddr = 0x20000000 + page - DataVA;
        entry->perms = uc_prot(UC_PROT_READ | UC_PROT_WRITE);
        return true;
    }
    return false;
}
void Probe::load(const void *bytes, size_t size)
{
    CHECK(size <= sizeof(code));
    code.fill(0);
    std::memcpy(code.data(), bytes, size);
    if (uc) {
        checked(uc_ctl_flush_tb(uc));
        checked(uc_ctl_flush_tlb(uc));
    }
    ++epoch;
}
void Probe::reference_run(NceDirectState &state, uint64_t stop)
{
    auto registers = [&](bool write) {
        for (unsigned i = 0; i < 31; ++i) {
            int id = i < 29 ? UC_ARM64_REG_X0 + i : i == 29 ? UC_ARM64_REG_X29
                                                            : UC_ARM64_REG_X30;
            checked(write ? uc_reg_write(uc, id, &state.x[i]) : uc_reg_read(uc, id, &state.x[i]));
        }
        for (unsigned i = 0; i < 32; ++i)
            checked(write ? uc_reg_write(uc, UC_ARM64_REG_Q0 + i, state.v[i]) : uc_reg_read(uc, UC_ARM64_REG_Q0 + i, state.v[i]));
        auto word = [&](int id, uint32_t &value) {
            uint64_t wide = value;
            checked(write ? uc_reg_write(uc, id, &wide) : uc_reg_read(uc, id, &wide));
            if (!write)
                value = (uint32_t)wide;
        };
        word(UC_ARM64_REG_NZCV, state.nzcv);
        word(UC_ARM64_REG_FPCR, state.fpcr);
        word(UC_ARM64_REG_FPSR, state.fpsr);
        checked(write ? uc_reg_write(uc, UC_ARM64_REG_SP, &state.sp) : uc_reg_read(uc, UC_ARM64_REG_SP, &state.sp));
        checked(write ? uc_reg_write(uc, UC_ARM64_REG_TPIDR_EL0, &state.tpidr_el0) : uc_reg_read(uc, UC_ARM64_REG_TPIDR_EL0, &state.tpidr_el0));
    };
    registers(true);
    checked(uc_emu_start(uc, state.pc, stop, 0, 0));
    registers(false);
    checked(uc_reg_read(uc, UC_ARM64_REG_PC, &state.pc));
}
NceDirectExit Probe::run(NceDirectState &state, uint64_t budget, uint64_t *fault)
{
    uint64_t address = 0;
    auto result = nce_direct_run(direct, &state, memory(), budget, &address);
    if (fault)
        *fault = address;
    return result;
}
void Probe::compare(NceDirectState initial, size_t instructions)
{
    auto expected = initial, actual = initial;
    reference = data;
    reference_run(expected, initial.pc + instructions * 4);
    auto result = run(actual);
    if (result != NCE_DIRECT_UNSUPPORTED || std::memcmp(&expected, &actual, sizeof(initial)) || reference != data) {
        std::fprintf(stderr, "difference result=%u pc=%llx/%llx flags=%x/%x fpsr=%x/%x\n", result,
                     expected.pc, actual.pc, expected.nzcv, actual.nzcv, expected.fpsr, actual.fpsr);
        for (unsigned i = 0; i < 31; ++i)
            if (expected.x[i] != actual.x[i])
                std::fprintf(stderr, "x%u %llx/%llx\n", i, expected.x[i], actual.x[i]);
        for (unsigned i = 0; i < instructions; ++i)
            std::fprintf(stderr, "%08x\n", code[i]);
        std::exit(1);
    }
}
