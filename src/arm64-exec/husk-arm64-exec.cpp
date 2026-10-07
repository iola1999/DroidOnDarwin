/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"
#include "husk-arm64-direct.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>

#include <dynarmic/interface/A64/a64.h>

using Dynarmic::A64::Vector;
using Dynarmic::HaltReason;

static bool dynarmic_supported(uint32_t insn)
{
    /* Admit baseline ARMv8 integer classes; optional ISA features stay with QEMU. */
    if ((insn & 0x1f000000) == 0x10000000) { // ADR / ADRP
        return true;
    }
    switch (insn & 0x1f800000) {
    case 0x11000000: // Add/subtract immediate
    case 0x12000000: // Logical immediate
    case 0x12800000: // Move wide
    case 0x13000000: // Bitfield
    case 0x13800000: // Extract
        return true;
    }
    if ((insn & 0x1f000000) == 0x0a000000 || // Logical shifted register
        (insn & 0x1f200000) == 0x0b000000 || // Add/subtract shifted register
        (insn & 0x1fe00000) == 0x0b200000 || // Add/subtract extended register
        (insn & 0x1fe0fc00) == 0x1a000000 || // Add/subtract with carry
        (insn & 0x3fe00410) == 0x3a400000 || // Conditional compare
        (insn & 0x3fe00800) == 0x1a800000 || // Conditional select
        (insn & 0x7fe00000) == 0x1b000000) { // Multiply-add/subtract
        return true;
    }
    switch (insn & 0x7fe0fc00) {
    case 0x1ac00800: case 0x1ac00c00: // UDIV / SDIV
    case 0x1ac02000: case 0x1ac02400: // LSLV / LSRV
    case 0x1ac02800: case 0x1ac02c00: // ASRV / RORV
        return true;
    }
    switch (insn & 0x7ffffc00) {
    case 0x5ac00000: case 0x5ac00400: case 0x5ac00800: // RBIT / REV16 / REV32
    case 0x5ac01000: case 0x5ac01400: // CLZ / CLS
        return true;
    }
    if ((insn & 0xfffffc00) == 0xdac00c00 || // REV64
        (insn & 0xffe00000) == 0x9b200000 || // Signed multiply long
        (insn & 0xffe00000) == 0x9ba00000 || // Unsigned multiply long
        (insn & 0xffe0fc00) == 0x9b407c00 || // SMULH
        (insn & 0xffe0fc00) == 0x9bc07c00) { // UMULH
        return true;
    }
    if ((insn & 0x7c000000) == 0x14000000 ||
        (insn & 0xff000010) == 0x54000000 ||
        (insn & 0x7e000000) == 0x34000000 ||
        (insn & 0x7e000000) == 0x36000000) {
        return true;
    }
    const uint32_t branch = insn & 0xfffffc1f;
    if (branch == 0xd61f0000 || branch == 0xd63f0000 || branch == 0xd65f0000) {
        return true;
    }
    if ((insn & 0x3e000000) == 0x28000000 && husk_direct_supported(insn)) return true;
    /* Scalar loads/stores admitted by the checked memory callbacks. */
    if ((insn & 0x04000000) == 0 && ((insn >> 22) & 3) <= 1) {
        if ((insn & 0x3b000000) == 0x39000000) {
            return true;
        }
        if ((insn & 0x3b200c00) == 0x38200800 && (insn & (1U << 14))) return true;
        if ((insn & 0x3b200000) == 0x38000000 && ((insn >> 10) & 3) != 2) {
            return true;
        }
    }
    return false;
}

bool husk_arm64_supported(uint32_t insn)
{
    static const bool direct = [] {
        const char *backend = std::getenv("HUSK_ARM64_BACKEND");
        return backend && !std::strcmp(backend, "direct");
    }();
    return direct ?
        husk_direct_supported(insn) : dynarmic_supported(insn);
}

struct HuskArm64Exec final : Dynarmic::A64::UserCallbacks {
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> direct{nullptr, husk_direct_destroy};
    using CacheEntry = Dynarmic::A64::UserConfig::MemoryAccessCacheEntry;
    static_assert(sizeof(CacheEntry) == 32);
    std::array<CacheEntry, 256> access_cache{};
    std::array<uint8_t, 256> populated_entries{};
    size_t populated_count = 0;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    std::array<uint8_t, 4096> code{};
    uint64_t page = UINT64_MAX;
    HuskArm64Memory memory{};
    uint64_t ticks = 0;
    bool fallback = false;
    std::optional<uint64_t> fallback_pc;

    HuskArm64Exec()
    {
        const char *backend = std::getenv("HUSK_ARM64_BACKEND");
        if (backend && std::strcmp(backend, "direct") == 0) {
            direct.reset(husk_direct_create());
            std::fprintf(stderr, "[husk-arm64] backend=direct\n");
            return;
        }
        Dynarmic::A64::UserConfig config{};
        config.callbacks = this;
        config.code_cache_size = 16 * 1024 * 1024;
        config.check_halt_on_memory_access = true;
        config.memory_access_cache = access_cache.data();
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        /* Re-entry must revalidate the supplied page before cached code executes. */
        config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        jit = std::make_unique<Dynarmic::A64::Jit>(config);
    }

    void CachePage(uint64_t address, bool write)
    {
        if (!memory.map_page || (address >> 56)) {
            return;
        }
        uint64_t page_index = address >> 12;
        auto &entry = access_cache[page_index & 255];
        if (entry.page_index == page_index && (write ? entry.write : entry.read)) {
            return;
        }
        void *host = memory.map_page(memory.opaque, address & ~UINT64_C(4095), write);
        if (!host) {
            return;
        }
        if (!entry.read && !entry.write) {
            populated_entries[populated_count++] = page_index & 255;
        }
        if (entry.page_index != page_index) {
            entry = {page_index, nullptr, nullptr, 0};
        }
        (write ? entry.write : entry.read) = host;
    }

    std::optional<uint32_t> MemoryReadCode(uint64_t address) override
    {
        if (address < page || address - page > code.size() - 4 || (address & 3)) {
            return std::nullopt;
        }
        uint32_t insn;
        std::memcpy(&insn, code.data() + address - page, sizeof(insn));
        return dynarmic_supported(insn) ? std::optional{insn} : std::nullopt;
    }

    template<class T> T Read(uint64_t address)
    {
        T value{};
        if (!memory.read(memory.opaque, address, &value, sizeof(value))) {
            fallback = true;
            jit->HaltExecution(HaltReason::MemoryAbort);
        } else {
            CachePage(address, false);
        }
        return value;
    }

    template<class T> void Write(uint64_t address, T value)
    {
        const bool overlaps_code = address <= page + code.size() - 1 &&
            (address >= page || page - address < sizeof(value));
        if (overlaps_code || !memory.write(memory.opaque, address, &value, sizeof(value))) {
            fallback = true;
            jit->HaltExecution(HaltReason::MemoryAbort);
        } else {
            CachePage(address, true);
        }
    }

    uint8_t MemoryRead8(uint64_t a) override { return Read<uint8_t>(a); }
    uint16_t MemoryRead16(uint64_t a) override { return Read<uint16_t>(a); }
    uint32_t MemoryRead32(uint64_t a) override { return Read<uint32_t>(a); }
    uint64_t MemoryRead64(uint64_t a) override { return Read<uint64_t>(a); }
    Vector MemoryRead128(uint64_t a) override { return Read<Vector>(a); }
    void MemoryWrite8(uint64_t a, uint8_t v) override { Write(a, v); }
    void MemoryWrite16(uint64_t a, uint16_t v) override { Write(a, v); }
    void MemoryWrite32(uint64_t a, uint32_t v) override { Write(a, v); }
    void MemoryWrite64(uint64_t a, uint64_t v) override { Write(a, v); }
    void MemoryWrite128(uint64_t a, Vector v) override { Write(a, v); }

    void Stop(uint64_t pc)
    {
        fallback = true;
        fallback_pc = pc;
        jit->HaltExecution();
    }
    void InterpreterFallback(uint64_t pc, size_t) override { Stop(pc); }
    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception) override { Stop(pc); }
    void CallSVC(uint32_t) override { Stop(jit->GetPC() - 4); }
    void AddTicks(uint64_t count) override { ticks -= std::min(count, ticks); }
    uint64_t GetTicksRemaining() override { return ticks; }
    uint64_t GetCNTPCT() override { return 0; }
};

HuskArm64Exec *husk_arm64_create(void)
{
    try {
        return new HuskArm64Exec;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[husk-arm64] create: %s\n", error.what());
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

void husk_arm64_destroy(HuskArm64Exec *context) { delete context; }

HuskArm64Exit husk_arm64_run(HuskArm64Exec *ctx, HuskArm64State *state,
                           uint64_t page, const void *bytes,
                           HuskArm64Memory memory, uint64_t budget)
{
    if (!ctx || !state || !bytes || !memory.read || !memory.write || !budget ||
        (page & 4095) || page > UINT64_MAX - 4095) {
        return HUSK_ARM64_ERROR;
    }
    try {
        if (ctx->direct) return husk_direct_run(ctx->direct.get(), state, page, bytes, memory, budget);
        if (ctx->page != page || std::memcmp(ctx->code.data(), bytes, 4096)) {
            ctx->jit->ClearCache();
            ctx->page = page;
            std::memcpy(ctx->code.data(), bytes, 4096);
        }
        ctx->memory = memory;
        /* Kernel execution between slices may change any address-space mapping. */
        for (size_t i = 0; i < ctx->populated_count; ++i) {
            ctx->access_cache[ctx->populated_entries[i]] = {UINT64_MAX, nullptr, nullptr, 0};
        }
        ctx->populated_count = 0;
        ctx->ticks = budget;
        ctx->fallback = false;
        ctx->fallback_pc.reset();
        /* ClearCache queues work for Run; its halt bit must survive re-entry. */
        ctx->jit->ClearHalt(HaltReason::UserDefined1 | HaltReason::MemoryAbort);
        std::array<uint64_t, 31> regs;
        std::memcpy(regs.data(), state->x, sizeof(state->x));
        ctx->jit->SetRegisters(regs);
        ctx->jit->SetSP(state->sp);
        ctx->jit->SetPC(state->pc);
        ctx->jit->SetPstate(state->nzcv & 0xf0000000);
        ctx->jit->Run();
        regs = ctx->jit->GetRegisters();
        std::memcpy(state->x, regs.data(), sizeof(state->x));
        state->sp = ctx->jit->GetSP();
        state->pc = ctx->fallback_pc.value_or(ctx->jit->GetPC());
        state->nzcv = ctx->jit->GetPstate() & 0xf0000000;
        return ctx->fallback ? HUSK_ARM64_FALLBACK : HUSK_ARM64_BUDGET;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[husk-arm64] execution: %s\n", error.what());
        return HUSK_ARM64_ERROR;
    } catch (...) {
        /* The owner must terminate this run: memory side effects may exist. */
        return HUSK_ARM64_ERROR;
    }
}
