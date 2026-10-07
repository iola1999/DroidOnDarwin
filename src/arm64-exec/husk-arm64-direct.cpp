/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <vector>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <TargetConditionals.h>
#include <oaknut/oaknut.hpp>
#if TARGET_OS_IPHONE
#include "husk-ios-jit.h"
#endif

using namespace oaknut;
using namespace oaknut::util;

namespace {
enum Stop : uint64_t { Budget, Fallback, ReadPage, WritePage, PrepareLoop };
struct Frame {
    HuskArm64State state;
    uint64_t budget, address, reason, loop_base;
};
static_assert(offsetof(Frame, state.pc) == 256);
static_assert(offsetof(Frame, state.nzcv) == 264);
static_assert(offsetof(Frame, budget) == 272);
static_assert(offsetof(Frame, address) == 280);
static_assert(offsetof(Frame, reason) == 288);
static_assert(offsetof(Frame, loop_base) == 296);
struct Mapping { uint64_t tag; void *read, *write; uint64_t reserved; };
static_assert(sizeof(Mapping) == 32);
extern "C" void husk_direct_enter(Frame *, void *, Mapping *);
extern "C" void husk_direct_leave();

bool resident(unsigned reg) { return reg < 16 || (reg >= 19 && reg < 26); }
int64_t sign_extend(uint32_t value, unsigned bits)
{
    return int64_t(uint64_t(value) << (64 - bits)) >> (64 - bits);
}

/* Only these validated encodings can execute as host instructions. */
enum Kind { Other, AddImm, MoveWide, AddReg, LogicalReg, LogicalImm, Bitfield,
            Extract, Unary, Binary, Select, Multiply, Adr, Branch, Conditional,
            CompareBranch, TestBranch, Indirect, Memory, Pair };
Kind decode(uint32_t w)
{
    bool sf = w >> 31;
    if ((w & 0x1f800000) == 0x11000000) return AddImm;
    if ((w & 0x1f800000) == 0x12800000 && ((w >> 29) & 3) != 1 &&
        (sf || !(w & (1U << 22)))) return MoveWide;
    if ((w & 0x1f200000) == 0x0b000000 && ((w >> 22) & 3) != 3 &&
        (sf || !(w & (1U << 15)))) return AddReg;
    if ((w & 0x1f000000) == 0x0a000000 &&
        (sf || !(w & (1U << 15)))) return LogicalReg;
    if ((w & 0x1f800000) == 0x12000000 && (sf || !(w & (1U << 22)))) {
        unsigned imms = (w >> 10) & 63;
        unsigned mask = (((w >> 22) & 1) << 6) | ((~imms) & 63);
        if (mask > 1) {
            unsigned len = 31 - __builtin_clz(mask);
            if ((imms & ((1U << len) - 1)) != ((1U << len) - 1)) return LogicalImm;
        }
    }
    if ((w & 0x1f800000) == 0x13000000 && ((w >> 29) & 3) != 3 &&
        ((w >> 22) & 1) == sf && (sf || !(w & ((1U << 21) | (1U << 15))))) return Bitfield;
    if ((w & 0x7fa00000) == 0x13800000 && ((w >> 22) & 1) == sf &&
        (sf || !(w & (1U << 15)))) return Extract;
    switch (w & 0x7ffffc00) {
    case 0x5ac00000: case 0x5ac00400: case 0x5ac00800:
    case 0x5ac01000: case 0x5ac01400: return Unary;
    }
    if ((w & 0xfffffc00) == 0xdac00c00) return Unary;
    switch (w & 0x7fe0fc00) {
    case 0x1ac00800: case 0x1ac00c00: case 0x1ac02000:
    case 0x1ac02400: case 0x1ac02800: case 0x1ac02c00: return Binary;
    }
    if ((w & 0x3fe00800) == 0x1a800000) return Select;
    if ((w & 0x7fe00000) == 0x1b000000) return Multiply;
    if ((w & 0x1f000000) == 0x10000000) return Adr;
    if ((w & 0x7c000000) == 0x14000000) return Branch;
    if ((w & 0xff000010) == 0x54000000) return Conditional;
    if ((w & 0x7e000000) == 0x34000000) return CompareBranch;
    if ((w & 0x7e000000) == 0x36000000) return TestBranch;
    auto br = w & 0xfffffc1f;
    // BR/BLR update guest BTYPE; QEMU must execute them and check the landing pad.
    if (br == 0xd65f0000) return Indirect;
    if ((w & 0x3e000000) == 0x28000000 && ((w >> 30) & 1) == 0) {
        unsigned rn = (w >> 5) & 31, rt = w & 31, rt2 = (w >> 10) & 31;
        bool writeback = (w >> 23) & 1;
        if (((w >> 22) & 1) && rt == rt2) return Other;
        if (writeback && rn != 31 && (rn == rt || rn == rt2)) return Other;
        return Pair;
    }
    if (!(w & 0x04000000) && ((w >> 22) & 3) <= 1) {
        if ((w & 0x3b000000) == 0x39000000) return Memory;
        if ((w & 0x3b200c00) == 0x38200800 && (w & (1U << 14))) return Memory;
        if ((w & 0x3b200000) == 0x38000000 && ((w >> 10) & 3) != 2) {
            // Writeback with Rn == Rt has constrained-unpredictable semantics.
            if ((w & 0xc00) && ((w >> 5) & 31) == (w & 31) && (w & 31) != 31)
                return Other;
            return Memory;
        }
    }
    return Other;
}
bool integer_kind(Kind kind) { return kind >= AddImm && kind <= Multiply; }
uint64_t target(uint32_t w, Kind kind, uint64_t pc)
{
    if (kind == Branch) return pc + sign_extend(w & 0x3ffffff, 26) * 4;
    unsigned bits = kind == TestBranch ? 14 : 19;
    return pc + sign_extend((w >> 5) & ((1U << bits) - 1), bits) * 4;
}
}

struct CompiledPage {
    struct Loop {
        size_t begin, end;
        unsigned base, extent = 0, alignment = 1;
        bool read = false, write = false;
        void *generic = nullptr, *fast = nullptr;
    };
    static constexpr size_t max_bytes = 1024 * 1024;
    std::array<uint32_t, 1024> source{};
    std::array<void *, 1024> entries{};
    std::vector<Loop> loops;

    size_t compile(uint64_t new_page, const void *bytes, uint32_t *executable, uint32_t *writable)
    {
        std::array<uint32_t, 1024> input;
        std::memcpy(input.data(), bytes, 4096);
        std::array<Label, 1025> labels;
        std::array<Label, 1024> exhausted;
        std::array<bool, 1025> leaders{};
        leaders[1024] = true;
        for (size_t i = 0; i < input.size(); ++i) {
            if (!(i % 32)) leaders[i] = true;
            Kind k = decode(input[i]);
            if (k >= Branch && k <= TestBranch) {
                uint64_t address = target(input[i], k, new_page + i * 4);
                if (address >= new_page && address - new_page < 4096)
                    leaders[(address - new_page) / 4] = true;
            }
            if (k == Other || (k >= Branch && k <= Indirect)) leaders[i + 1] = true;
        }
        loops.clear();
        const char *loop_setting = std::getenv("HUSK_ARM64_LOOP_MAP");
        if (!loop_setting || std::strcmp(loop_setting, "0")) {
            for (size_t end = 1; end < input.size(); ++end) {
                if (decode(input[end]) != Conditional) continue;
                uint64_t start = target(input[end], Conditional, new_page + end * 4);
                if (start < new_page || start >= new_page + end * 4) continue;
                size_t begin = (start - new_page) / 4;
                if (end - begin >= 32) continue;
                Loop loop{begin, end, 32};
                bool valid = true;
                for (size_t j = begin; j < end; ++j) {
                    auto w = input[j];
                    auto kind = decode(w);
                    if (kind == Memory && (w & 0x3b000000) == 0x39000000) {
                        unsigned base = (w >> 5) & 31, size = 1U << (w >> 30);
                        if (base == 31 || (loop.base != 32 && loop.base != base)) valid = false;
                        loop.base = base;
                        loop.extent = std::max(loop.extent, (((w >> 10) & 4095) + 1) * size);
                        loop.alignment = std::max(loop.alignment, size);
                        (w & (1U << 22) ? loop.read : loop.write) = true;
                    } else if (!integer_kind(kind)) {
                        valid = false;
                    }
                }
                if (loop.base == 32 || loop.extent > 4096) valid = false;
                for (size_t j = begin; j < end; ++j) {
                    // All memory addresses must remain invariant for every iteration.
                    auto w = input[j];
                    bool writes_reg = decode(w) != Memory || (w & (1U << 22));
                    if (writes_reg && (w & 31) == loop.base) valid = false;
                }
                if (valid) loops.push_back(loop);
            }
        }
        std::vector<uint32_t> output;
        VectorCodeGenerator c(output, executable);
        auto store_pc = [&](uint64_t pc) { c.MOV(X17, pc); c.STR(X17, X27, 256); };
        auto leave = [&](Stop reason) {
            c.MOV(X17, uint64_t(reason)); c.STR(X17, X27, 288);
            c.MOV(X16, uint64_t(&husk_direct_leave)); c.BR(X16);
        };
        auto exit = [&](uint64_t pc, Stop reason) { store_pc(pc); leave(reason); };
        auto load = [&](unsigned r, XReg to, bool sp = false) {
            if (resident(r)) c.MOV(to, XReg(r));
            else if (r == 31 && !sp) c.MOV(to, 0);
            else c.LDR(to, X27, r * 8);
        };
        auto save = [&](unsigned r, XReg from, bool sp = false) {
            if (resident(r)) c.MOV(XReg(r), from);
            else if (r != 31 || sp) c.STR(from, X27, r * 8);
        };
        auto jump = [&](uint64_t pc) {
            if (pc >= new_page && pc - new_page < 4096) c.B(labels[(pc - new_page) / 4]);
            else exit(pc, Fallback);
        };
        auto integer = [&](uint32_t w, Kind kind, unsigned third_scratch) {
            unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31, ra = (w >> 10) & 31;
            std::array<int, 32> aliases;
            aliases.fill(-1);
            unsigned scratch = 0;
            unsigned temps[] = {16, 17, third_scratch};
            auto operand = [&](unsigned r, bool sp = false) -> unsigned {
                if (resident(r) || (r == 31 && !sp)) return r;
                if (aliases[r] == -1) {
                    aliases[r] = temps[scratch++];
                    c.LDR(XReg(aliases[r]), X27, r * 8);
                }
                return aliases[r];
            };
            bool set_flags = w & (1U << 29);
            bool two_sources = kind == AddReg || kind == LogicalReg || kind == Extract ||
                               kind == Binary || kind == Select || kind == Multiply;
            unsigned n = 0, m = 0, a = 0;
            if (kind != MoveWide) n = operand(rn, kind == AddImm);
            if (two_sources) m = operand(rm);
            if (kind == Multiply) a = operand(ra);
            bool dest_sp = (kind == AddImm && !set_flags) ||
                           (kind == LogicalImm && ((w >> 29) & 3) != 3);
            unsigned d;
            if (kind == Multiply && !resident(rd) && rd != 31 && aliases[rd] == -1) {
                // Multiply reads its sources before writing Rd; a source scratch
                // can therefore hold the result without a fourth scratch register.
                d = temps[0];
            } else {
                d = operand(rd, dest_sp);
            }
            w = (w & ~31U) | d;
            if (kind != MoveWide) w = (w & ~(31U << 5)) | (n << 5);
            if (two_sources) w = (w & ~(31U << 16)) | (m << 16);
            if (kind == Multiply) w = (w & ~(31U << 10)) | (a << 10);
            c.dw(w);
            if (!resident(rd) && (rd != 31 || dest_sp)) c.STR(XReg(d), X27, rd * 8);
        };

        for (size_t i = 0; i < input.size(); ++i) {
            uint64_t pc = new_page + i * 4;
            c.l(labels[i]);
            if (leaders[i]) {
                size_t end = i + 1;
                while (!leaders[end]) ++end;
                c.SUB(X29, X29, end - i);
                c.TBNZ(X29, 63, exhausted[i]);
            }
            // Slow-path re-entry already charged this block's execution budget.
            entries[i] = c.xptr<void *>();
            for (auto &loop : loops) {
                if (loop.begin == i) {
                    exit(pc, PrepareLoop);
                    loop.generic = c.xptr<void *>();
                }
            }
            uint32_t w = input[i];
            unsigned rd = w & 31, rn = (w >> 5) & 31;
            Kind kind = decode(w);
            if (integer_kind(kind)) {
                integer(w, kind, 26);
            } else if (kind == Adr) {
                int64_t imm = sign_extend(((w >> 29) & 3) | ((w >> 3) & 0x1ffffc), 21);
                uint64_t value = w >> 31 ? (pc & ~UINT64_C(4095)) + imm * 4096 : pc + imm;
                c.MOV(X16, value); save(rd, X16);
            } else if (kind >= Branch && kind <= TestBranch) {
                uint64_t dest = target(w, kind, pc);
                if (kind == Branch) {
                    if (w >> 31) { c.MOV(X16, pc + 4); save(30, X16); }
                    jump(dest);
                } else {
                    Label outside, next;
                    bool in_page = dest >= new_page && dest - new_page < 4096;
                    bool near_branch = in_page && kind != TestBranch;
                    Label &taken = near_branch ? labels[(dest - new_page) / 4] : outside;
                    if (kind == Conditional) c.B(Cond(w & 15), taken);
                    else {
                        load(rd, X16);
                        if (kind == TestBranch) {
                            unsigned bit = ((w >> 26) & 32) | ((w >> 19) & 31);
                            if (w & (1U << 24)) c.TBNZ(X16, bit, taken); else c.TBZ(X16, bit, taken);
                        } else if (w >> 31) {
                            if (w & (1U << 24)) c.CBNZ(X16, taken); else c.CBZ(X16, taken);
                        } else {
                            if (w & (1U << 24)) c.CBNZ(W16, taken); else c.CBZ(W16, taken);
                        }
                    }
                    if (!near_branch) { c.B(next); c.l(outside); jump(dest); c.l(next); }
                }
            } else if (kind == Indirect) {
                load(rn, X16);
                c.STR(X16, X27, 256);
                // Dynamic targets return to the owner for execute-permission checks.
                leave(Fallback);
            } else if (kind == Memory || kind == Pair) {
                bool write = !(w & (1U << 22));
                bool pair = kind == Pair;
                bool reg_offset = !pair && (w & 0x3b200c00) == 0x38200800;
                bool indexed = !pair && !reg_offset && (w & 0x3b000000) != 0x39000000;
                unsigned mode = pair ? (((w >> 23) & 1) ? ((w >> 23) & 3) : 0) :
                                indexed ? (w >> 10) & 3 : 0;
                unsigned size_log2 = pair ? 2 + (w >> 31) : w >> 30;
                int64_t offset = pair ? sign_extend((w >> 15) & 127, 7) * (1 << size_log2) :
                                 reg_offset ? 0 : indexed ? sign_extend((w >> 12) & 511, 9) :
                                 ((w >> 10) & 4095) << size_log2;
                Label miss, invalid, done;
                c.MRS(X30, SystemReg::NZCV);
                if (pair) c.STR(W30, X27, 264);
                load(rn, X16, true);
                if (rn == 31) { c.TST(X16, 15); c.B(NE, invalid); }
                if (reg_offset) {
                    load((w >> 16) & 31, X17);
                    unsigned option = (w >> 13) & 7;
                    if (option == 2) c.UBFX(X17, X17, 0, 32);
                    if (option == 6) c.SXTW(X17, W17);
                    c.ADD(X16, X16, X17, LSL, (w & (1U << 12)) ? size_log2 : 0);
                }
                if (mode != 1 && offset) {
                    if (offset > 4095) { c.MOV(X17, uint64_t(offset)); c.ADD(X16, X16, X17); }
                    else if (offset > 0) c.ADD(X16, X16, offset);
                    else c.SUB(X16, X16, -offset);
                }
                c.LSR(X17, X16, 56); c.CBNZ(X17, invalid);
                if (size_log2) { c.TST(X16, (1U << size_log2) - 1); c.B(NE, invalid); }
                if (pair) {
                    c.AND(X17, X16, 4095);
                    c.CMP(X17, 4096 - (2U << size_log2)); c.B(HI, invalid);
                }
                c.UBFX(X17, X16, 12, 8);
                c.ADD(X17, X28, X17, LSL, 5);
                c.LDR(X26, X17);
                c.CMP(X26, X16, LSR, 12); c.B(NE, miss);
                c.LDR(X17, X17, write ? 16 : 8); c.CBZ(X17, miss);
                c.AND(X26, X16, 4095); c.ADD(X17, X17, X26);
                if (write) load(rd, X26);
                if (pair) {
                    unsigned rt2 = (w >> 10) & 31;
                    if (write) load(rt2, X30);
                    // Preserve a single architectural pair access after validating
                    // both elements, including their common page boundary.
                    c.dw(((size_log2 - 2) << 31) | 0x29000000 | (write ? 0 : 1U << 22) |
                         (30 << 10) | (17 << 5) | 26);
                    if (!write) { save(rd, X26); save(rt2, X30); }
                    c.LDR(W30, X27, 264);
                } else {
                    c.dw((size_log2 << 30) | 0x39000000 | (write ? 0 : 1U << 22) | (17 << 5) | 26);
                    if (!write) save(rd, X26);
                }
                if (mode) {
                    if (mode == 1 && offset) {
                        if (offset > 0) c.ADD(X16, X16, offset); else c.SUB(X16, X16, -offset);
                    }
                    save(rn, X16, true);
                }
                c.MSR(SystemReg::NZCV, X30); c.B(done);
                c.l(miss);
                c.MSR(SystemReg::NZCV, X30); c.STR(X16, X27, 280);
                exit(pc, write ? WritePage : ReadPage);
                c.l(invalid); c.MSR(SystemReg::NZCV, X30); exit(pc, Fallback);
                c.l(done);
            } else {
                exit(pc, Fallback);
            }
            if (i % 32 == 31) {
                // TBNZ has a 32 KiB reach. Keep budget exits near their callers.
                Label next;
                c.B(next);
                for (size_t j = i - 31; j <= i; ++j) {
                    if (leaders[j]) { c.l(exhausted[j]); exit(new_page + j * 4, Budget); }
                }
                c.l(next);
            }
        }
        c.l(labels[1024]);
        exit(new_page + 4096, Fallback);
        for (auto &loop : loops) {
            loop.fast = c.xptr<void *>();
            c.LDR(X26, X27, 296);
            Label again, budget_end;
            c.l(again);
            c.SUB(X29, X29, loop.end - loop.begin + 1);
            c.TBNZ(X29, 63, budget_end);
            for (size_t j = loop.begin; j < loop.end; ++j) {
                auto w = input[j];
                auto kind = decode(w);
                if (kind == Memory) {
                    unsigned rd = w & 31;
                    bool write = !(w & (1U << 22));
                    unsigned host_reg = resident(rd) || rd == 31 ? rd : 16;
                    if (write && !resident(rd) && rd != 31) load(rd, X16);
                    // A bounded loop keeps this validated host mapping in x26.
                    c.dw((w & ~1023U) | (26 << 5) | host_reg);
                    if (!write && !resident(rd) && rd != 31) save(rd, X16);
                } else {
                    integer(w, kind, 30);
                }
            }
            c.B(Cond(input[loop.end] & 15), again);
            jump(new_page + (loop.end + 1) * 4);
            c.l(budget_end);
            exit(new_page + loop.begin * 4, Budget);
        }
        if (output.size() * 4 > max_bytes) throw std::length_error("direct code page exceeds pool");
#if !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(0);
#endif
        std::memcpy(writable, output.data(), output.size() * 4);
#if !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(1);
#endif
        sys_icache_invalidate(executable, output.size() * 4);
        source = input;
        return output.size() * 4;
    }
};

struct HuskArm64Direct {
    static constexpr size_t capacity = 32 * 1024 * 1024;
    uint32_t *executable;
    uint32_t *writable;
#if TARGET_OS_IPHONE
    HuskDualMapping mapping{};
#endif
    size_t used = 0;
    std::unordered_map<uint64_t, std::unique_ptr<CompiledPage>> pages;
    std::array<Mapping, 256> mappings{};
    std::array<uint8_t, 256> populated{};
    size_t populated_count = 0;
    bool profile = std::getenv("HUSK_ARM64_STATS") != nullptr;
    uint64_t calls = 0, compiles = 0, flushes = 0, compile_ns = 0, run_ns = 0;
    std::chrono::steady_clock::time_point report = std::chrono::steady_clock::now();

    HuskArm64Direct()
    {
#if TARGET_OS_IPHONE
        mapping = husk_ios_jit_acquire_el0(capacity);
        if (!mapping.rw_addr) throw std::runtime_error("EL0 JIT partition unavailable; enable JIT before boot");
        executable = reinterpret_cast<uint32_t *>(mapping.rx_addr);
        writable = reinterpret_cast<uint32_t *>(mapping.rw_addr);
#else
        executable = static_cast<uint32_t *>(mmap(nullptr, capacity,
            PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
        if (executable == MAP_FAILED) throw std::bad_alloc();
        writable = executable;
#endif
    }
    ~HuskArm64Direct()
    {
#if TARGET_OS_IPHONE
        husk_ios_jit_release_el0(&mapping);
#else
        munmap(executable, capacity);
#endif
    }

    CompiledPage &code_page(uint64_t page, const void *bytes)
    {
        auto found = pages.find(page);
        // QEMU rechecks execute permission; bytes also distinguish address spaces
        // and writable aliases. Data mappings are validated again on every run.
        if (found != pages.end() && !std::memcmp(found->second->source.data(), bytes, 4096))
            return *found->second;
        if (capacity - used < CompiledPage::max_bytes) {
            pages.clear();
            used = 0;
            ++flushes;
        }
        auto compiled = std::make_unique<CompiledPage>();
        used += compiled->compile(page, bytes, executable + used / 4, writable + used / 4);
        ++compiles;
        auto &entry = pages[page];
        entry = std::move(compiled);
        return *entry;
    }
};

HuskArm64Direct *husk_direct_create() { return new HuskArm64Direct; }
bool husk_direct_supported(uint32_t instruction) { return decode(instruction) != Other; }
void husk_direct_destroy(HuskArm64Direct *ctx) { delete ctx; }

HuskArm64Exit husk_direct_run(HuskArm64Direct *ctx, HuskArm64State *state,
                            uint64_t page, const void *bytes,
                            HuskArm64Memory memory, uint64_t budget)
{
    auto started = ctx->profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (state->pc < page || state->pc - page >= 4096 || (state->pc & 3))
        return HUSK_ARM64_FALLBACK;
    auto previous_compiles = ctx->compiles;
    auto &code = ctx->code_page(page, bytes);
    if (ctx->profile && previous_compiles != ctx->compiles)
        ctx->compile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
    for (size_t i = 0; i < ctx->populated_count; ++i)
        ctx->mappings[ctx->populated[i]] = {UINT64_MAX, nullptr, nullptr, 0};
    ctx->populated_count = 0;
    Frame frame{*state, std::min(budget, uint64_t(INT64_MAX)), 0, 0, 0};
    void *next = code.entries[(state->pc - page) / 4];
    for (;;) {
        husk_direct_enter(&frame, next, ctx->mappings.data());
        if (frame.reason == Budget || frame.reason == Fallback) break;
        if (frame.reason == PrepareLoop) {
            const auto &loop = *std::find_if(code.loops.begin(), code.loops.end(), [&](const auto &l) {
                return page + l.begin * 4 == frame.state.pc;
            });
            next = loop.generic;
            uint64_t base = frame.state.x[loop.base];
            if (memory.map_page && !(base >> 56) && !(base & (loop.alignment - 1)) &&
                (base & 4095) + loop.extent <= 4096 &&
                (!loop.write || (base & ~UINT64_C(4095)) != page)) {
                void *read = loop.read ? memory.map_page(memory.opaque, base & ~UINT64_C(4095), false) : nullptr;
                void *write = loop.write ? memory.map_page(memory.opaque, base & ~UINT64_C(4095), true) : nullptr;
                if ((!loop.read || read) && (!loop.write || write) && (!read || !write || read == write)) {
                    frame.loop_base = uint64_t(write ? write : read) + (base & 4095);
                    next = loop.fast;
                }
            }
            continue;
        }
        bool write = frame.reason == WritePage;
        uint64_t base = frame.address & ~UINT64_C(4095);
        void *host = memory.map_page ? memory.map_page(memory.opaque, base, write) : nullptr;
        if (!host || (write && base == page)) { frame.reason = Fallback; break; }
        uint64_t tag = base >> 12;
        auto &entry = ctx->mappings[tag & 255];
        if (!entry.read && !entry.write) ctx->populated[ctx->populated_count++] = tag & 255;
        if (entry.tag != tag) entry = {tag, nullptr, nullptr, 0};
        (write ? entry.write : entry.read) = host;
        next = code.entries[(frame.state.pc - page) / 4];
    }
    *state = frame.state;
    if (ctx->profile) {
        auto now = std::chrono::steady_clock::now();
        ++ctx->calls;
        ctx->run_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(now - started).count();
        if (now - ctx->report > std::chrono::seconds(5)) {
            std::fprintf(stderr, "[husk-direct-stats] calls=%llu compiles=%llu compile_ms=%.3f run_ms=%.3f pages=%zu code_kb=%zu flushes=%llu pc=%llx\n",
                (unsigned long long)ctx->calls, (unsigned long long)ctx->compiles,
                ctx->compile_ns / 1e6, ctx->run_ns / 1e6, ctx->pages.size(), ctx->used / 1024,
                (unsigned long long)ctx->flushes, (unsigned long long)state->pc);
            ctx->report = now;
        }
    }
    return frame.reason == Budget ? HUSK_ARM64_BUDGET : HUSK_ARM64_FALLBACK;
}
