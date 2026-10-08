/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include "husk-arm64-layout.h"
#include "husk-arm64-fastmem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <signal.h>
#include <TargetConditionals.h>
#include <oaknut/oaknut.hpp>
#if TARGET_OS_IPHONE
#include "husk-ios-jit.h"
#endif

using namespace oaknut;
using namespace oaknut::util;

struct CompiledPage;
namespace {
constexpr uint32_t CompileFastmem = 16, CompileLanding = 32, CompileInlineAssists = 256;
constexpr uint32_t CompileExclusive = 512, CompileTail = 1024;
constexpr uint32_t CompilePauthVerify = 2048;
constexpr uint32_t CompileKernelReads = 4096;
constexpr uint32_t CompileSharedStubs = 8192;
constexpr uint32_t CompileAes = 16384, CompileSha1 = 32768, CompileSha256 = 65536;
constexpr uint32_t CompilePauthNop = 131072;
constexpr uint32_t CompileRelocatable = 262144;
enum Stop : uint64_t { Budget, Fallback, ReadPage, WritePage, PrepareLoop, CodeBranch,
                       FastReadPage, FastWritePage, PauthCheck };
struct FaultSite {
    uintptr_t instruction, slow, patch, checked;
    unsigned bytes;
    bool write;
};
struct Frame {
    HuskArm64State state;
    uint64_t budget, address, reason, loop_base;
    void *code_routes;
    void *call_routes;
    const uintptr_t *read_windows, *write_windows;
    void *pauth_cache;
    const FaultSite *fault_sites;
    size_t fault_count, fault_index;
    uint64_t code_page;
    CompiledPage *code;
    const HuskArm64Pauth *pauth;
    const HuskArm64KernelReads *kernel_reads;
    void (*exit_stub)(), (*branch_stub)();
    uint64_t cache_clock;
};
static_assert(offsetof(Frame, cache_clock) == HUSK_CACHE_CLOCK);
static_assert(offsetof(Frame, exit_stub) == HUSK_EXIT_STUB);
static_assert(offsetof(Frame, branch_stub) == HUSK_BRANCH_STUB);
static_assert(offsetof(Frame, code_page) == HUSK_CODE_PAGE);
static_assert(offsetof(Frame, code) == HUSK_CODE);
static_assert(CodeBranch == 5 && PauthCheck < 16);
static_assert(offsetof(Frame, kernel_reads) == HUSK_KERNEL_READS);
static_assert(sizeof(HuskArm64KernelReads) == 24);
static_assert(offsetof(Frame, pauth) == HUSK_NATIVE_PAUTH);
static_assert(sizeof(HuskArm64PauthKey) == 32);
static_assert(offsetof(HuskArm64PauthKey, mask) == 16);
static_assert(offsetof(HuskArm64PauthKey, enabled) == 24);
static_assert(offsetof(Frame, state.pc) == HUSK_PC);
static_assert(offsetof(Frame, state.nzcv) == HUSK_NZCV);
static_assert(offsetof(Frame, state.fpcr) == HUSK_FPCR);
static_assert(offsetof(Frame, state.fpsr) == HUSK_FPSR);
static_assert(offsetof(Frame, state.access_flags) == HUSK_ACCESS_FLAGS);
static_assert(offsetof(Frame, state.tpidr_el0) == HUSK_TPIDR);
static_assert(offsetof(Frame, state.v) == HUSK_VECTORS);
static_assert(offsetof(Frame, state.exclusive_address) == HUSK_EXCLUSIVE_ADDR);
static_assert(offsetof(Frame, state.exclusive_value) == HUSK_EXCLUSIVE_VALUE);
static_assert(offsetof(Frame, budget) == HUSK_BUDGET);
static_assert(offsetof(Frame, address) == HUSK_ADDRESS);
static_assert(offsetof(Frame, reason) == HUSK_REASON);
static_assert(offsetof(Frame, loop_base) == HUSK_LOOP_BASE);
static_assert(offsetof(Frame, code_routes) == HUSK_CODE_ROUTES);
static_assert(offsetof(Frame, call_routes) == HUSK_CALL_ROUTES);
static_assert(offsetof(Frame, read_windows) == HUSK_READ_WINDOWS);
static_assert(offsetof(Frame, write_windows) == HUSK_WRITE_WINDOWS);
static_assert(offsetof(Frame, pauth_cache) == HUSK_PAUTH_CACHE);
struct Mapping { uint64_t read_tag, read_addend, write_tag, write_addend; };
constexpr Mapping EmptyMapping{UINT64_MAX, 0, UINT64_MAX, 0};
static_assert(sizeof(Mapping) == 32 && offsetof(Mapping, write_tag) == 16);
struct CodeRoute { uint64_t tag; void **entries; CompiledPage *code; uint64_t reserved; };
struct CallRoute {
    uint64_t target;
    void *entry;
    uint64_t page;
    CompiledPage *code;
    uint64_t source_tag;
    uint32_t btype;
    uint32_t reserved[5];
};
unsigned call_slot(uint64_t target, bool jump) {
    return ((target >> 2) ^ (target >> 10)) % 256 + (jump ? 256 : 0);
}
struct SourceRoute { const void *source; CompiledPage *code; uint32_t flags; uint64_t version; };
struct PauthResult { uint64_t value, modifier, result, instruction; };
static_assert(sizeof(Mapping) == 32);
static_assert(sizeof(CodeRoute) == 32);
static_assert(offsetof(CodeRoute, entries) == 8 && offsetof(CodeRoute, code) == 16);
static_assert(sizeof(CallRoute) == 64);
static_assert(sizeof(PauthResult) == 32);
extern "C" void husk_direct_enter(Frame *, void *, Mapping *);
extern "C" void husk_direct_leave();
extern "C" void husk_direct_exit_stub();
extern "C" void husk_direct_branch_stub();
extern "C" void husk_direct_pauth();

/* Fastmem currently executes on one vCPU. Identity is checked against the
 * saved native frame register before the handler dereferences that frame. */
std::atomic<Frame *> active_fastmem;
static_assert(decltype(active_fastmem)::is_always_lock_free);
struct sigaction previous_bus, previous_segv;

void fastmem_signal(int signal, siginfo_t *info, void *opaque)
{
    auto *uc = static_cast<ucontext_t *>(opaque);
    Frame *frame = active_fastmem.load(std::memory_order_relaxed);
    if (frame && uc && info && uc->uc_mcontext->__ss.__x[27] == uintptr_t(frame)) {
        uintptr_t pc = uc->uc_mcontext->__ss.__pc;
        size_t low = 0, high = frame->fault_count;
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (frame->fault_sites[middle].instruction < pc) low = middle + 1;
            else high = middle;
        }
        if (low < frame->fault_count && frame->fault_sites[low].instruction == pc) {
            const auto &site = frame->fault_sites[low];
            uint64_t address = uc->uc_mcontext->__ss.__x[16];
            const auto *windows = site.write ? frame->write_windows : frame->read_windows;
            if (HuskArm64Fastmem::compact_address(address, frame->state.access_flags & 8)) {
                uintptr_t host = windows[address >> HuskArm64Fastmem::segment_bits] +
                                 (address & (HuskArm64Fastmem::segment_size - 1));
                uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
                if (host == uc->uc_mcontext->__ss.__x[17] && fault >= host && fault - host < site.bytes) {
                    frame->fault_index = low;
                    uc->uc_mcontext->__ss.__pc = site.slow;
                    return;
                }
            }
        }
    }
    auto &previous = signal == SIGBUS ? previous_bus : previous_segv;
    if (previous.sa_handler == SIG_DFL || previous.sa_handler == SIG_IGN) {
        sigaction(signal, &previous, nullptr);
        raise(signal);
    } else if (previous.sa_flags & SA_SIGINFO) {
        previous.sa_sigaction(signal, info, opaque);
    } else {
        previous.sa_handler(signal);
    }
}

void install_fastmem_signals()
{
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action{};
        action.sa_sigaction = fastmem_signal;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGBUS, &action, &previous_bus))
            throw std::runtime_error("fastmem SIGBUS handler installation failed");
        if (sigaction(SIGSEGV, &action, &previous_segv)) {
            sigaction(SIGBUS, &previous_bus, nullptr);
            throw std::runtime_error("fastmem SIGSEGV handler installation failed");
        }
    });
    /* QEMU worker threads inherit a blocked SIGBUS on Darwin. A synchronous
     * protection fault needs delivery on this executing thread. */
    sigset_t faults;
    sigemptyset(&faults);
    sigaddset(&faults, SIGBUS);
    sigaddset(&faults, SIGSEGV);
    if (pthread_sigmask(SIG_UNBLOCK, &faults, nullptr))
        throw std::runtime_error("fastmem fault signal delivery failed");
}

bool resident(unsigned reg) { return reg < 16 || (reg >= 19 && reg < 26); }
int64_t sign_extend(uint32_t value, unsigned bits)
{
    return int64_t(uint64_t(value) << (64 - bits)) >> (64 - bits);
}

/* Only these validated encodings can execute as host instructions. */
enum Kind { Other, AddImm, MoveWide, AddReg, LogicalReg, LogicalImm, Bitfield,
            Extract, Unary, Binary, Crc, Select, Multiply, AddExt, Carry, Adr, Branch, Conditional,
            CompareBranch, TestBranch, Indirect, Memory, Pair, Vector, CryptoAes, CryptoSha1, CryptoSha256,
            VectorFromGpr, GprFromVector, FpFromGpr, GprFromFp, Compare, Hint, SystemRead, Assist, Atomic,
            Pauth, Barrier, Bti, ExclusiveLoad, ExclusiveStore, ClearExclusive, Structure, KernelRead };

unsigned structure_registers(uint32_t word)
{
    switch ((word >> 12) & 15) {
    case 0: case 2: return 4;
    case 4: case 6: return 3;
    case 7: return 1;
    case 8: case 10: return 2;
    default: return 0;
    }
}

unsigned pauth_register(uint32_t word) { return (word & 0xffffff3f) == 0xd503211f ? 17 : 30; }
unsigned pauth_slot(uint32_t word, uint64_t value, uint64_t modifier)
{
    return ((value >> 2) ^ (value >> 32) ^ (modifier >> 4) ^ (word >> 5)) & 255;
}
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
    switch (w & 0xffe0fc00) {
    case 0x1ac04000: case 0x1ac04400: case 0x1ac04800: case 0x9ac04c00:
    case 0x1ac05000: case 0x1ac05400: case 0x1ac05800: case 0x9ac05c00: return Crc;
    }
    if ((w & 0x3fe00800) == 0x1a800000) return Select;
    if ((w & 0x7fe00000) == 0x1b000000) return Multiply;
    if ((w & 0xffe00000) == 0x9b200000 || (w & 0xffe00000) == 0x9ba00000 ||
        (w & 0xffe0fc00) == 0x9b407c00 || (w & 0xffe0fc00) == 0x9bc07c00) return Multiply;
    if ((w & 0x1fe00000) == 0x0b200000 && ((w >> 10) & 7) <= 4) return AddExt;
    if ((w & 0x1fe0fc00) == 0x1a000000) return Carry;
    if ((w & 0x3fe00410) == 0x3a400000) return Compare;
    if (w == 0xd503201f) return Hint;
    if ((w & 0xfffff0ff) == 0xd503305f) return ClearExclusive;
    switch (w & 0xffffffe0) {
    case 0xd53bd040: case 0xd53b4200: case 0xd53b4400: case 0xd53b4420: return SystemRead;
    case 0xd538d080: case 0xd5384100: case 0xd53b4220: return KernelRead;
    case 0xd53be000: case 0xd53be020: case 0xd53be040:
    case 0xd53be0a0: case 0xd53be0c0: return Assist;
    }
    if ((w & 0x1f000000) == 0x10000000) return Adr;
    if ((w & 0x7c000000) == 0x14000000) return Branch;
    if ((w & 0xff000010) == 0x54000000) return Conditional;
    if ((w & 0x7e000000) == 0x34000000) return CompareBranch;
    if ((w & 0x7e000000) == 0x36000000) return TestBranch;
    auto br = w & 0xfffffc1f;
    // BR/BLR retain the owner's BTYPE and destination checks through its assist.
    if (br == 0xd61f0000 || br == 0xd63f0000) return Assist;
    if (br == 0xd65f0000) return Indirect;
    if (w == HUSK_BULK_INSN || husk_arm64_mops_prologue(w)) return Assist;
    switch (w) {
    case 0xd50320ff:
    case 0xd503211f: case 0xd503215f: case 0xd503219f: case 0xd50321df:
    case 0xd503231f: case 0xd503233f: case 0xd503235f: case 0xd503237f:
    case 0xd503239f: case 0xd50323bf: case 0xd50323df: case 0xd50323ff:
        return Pauth;
    case 0xd503241f: case 0xd503245f: case 0xd503249f: case 0xd50324df:
        return Bti;
    }
    if ((w & 0xfffff0ff) == 0xd50330bf || (w & 0xfffff0ff) == 0xd503309f)
        return Barrier;
    bool vector = w & 0x04000000;
    if ((w & 0x3fff7c00) == 0x085f7c00) return ExclusiveLoad;
    if ((w & 0x3fe07c00) == 0x08007c00) return ExclusiveStore;
    if ((w & 0x3fa07c00) == 0x08a07c00 ||
        ((w & 0x3f200c00) == 0x38200000 && ((w >> 12) & 15) <= 8)) return Atomic;
    if ((w & 0x3fbffc00) == 0x089ffc00) return Memory; // LDAR/STLR, all integer sizes
    if ((w & 0x3a000000) == 0x28000000 &&
        (vector ? (w >> 30) < 3 : ((w >> 30) & 1) == 0)) {
        unsigned rn = (w >> 5) & 31, rt = w & 31, rt2 = (w >> 10) & 31;
        bool writeback = (w >> 23) & 1;
        if (((w >> 22) & 1) && rt == rt2) return Other;
        if (!vector && writeback && rn != 31 && (rn == rt || rn == rt2)) return Other;
        return Pair;
    }
    unsigned opc = (w >> 22) & 3, size = w >> 30;
    bool scalar_memory = opc <= 1 || (opc == 2 && size < 3) || (opc == 3 && size < 2);
    if (vector ? size + ((opc >> 1) * 4) <= 4 : scalar_memory) {
        if ((w & 0x3b000000) == 0x39000000) return Memory;
        if ((w & 0x3b200c00) == 0x38200800 && (w & (1U << 14))) return Memory;
        if ((w & 0x3b200000) == 0x38000000 && ((w >> 10) & 3) != 2) {
            // Writeback with Rn == Rt has constrained-unpredictable semantics.
            if (!vector && (w & 0xc00) && ((w >> 5) & 31) == (w & 31) && (w & 31) != 31)
                return Other;
            return Memory;
        }
    }

    // Baseline Advanced SIMD encodings only. Size and Q restrictions reject
    // reserved encodings before any guest word can run on the host CPU.
    unsigned q = (w >> 30) & 1, element = (w >> 22) & 3;
    switch (w & 0xfffffc00) {
    case 0x4e284800: case 0x4e285800: case 0x4e286800: case 0x4e287800: return CryptoAes;
    case 0x5e280800: case 0x5e281800: return CryptoSha1;
    case 0x5e282800: return CryptoSha256;
    }
    switch (w & 0xffe0fc00) {
    case 0x5e000000: case 0x5e001000: case 0x5e002000: case 0x5e003000: return CryptoSha1;
    case 0x5e004000: case 0x5e005000: case 0x5e006000: return CryptoSha256;
    }
    if ((w & 0xbf200000) == 0x0c000000 && structure_registers(w)) {
        unsigned opcode = (w >> 12) & 15;
        bool sequential = opcode == 2 || opcode == 6 || opcode == 7 || opcode == 10;
        if (((w & (1U << 23)) || !((w >> 16) & 31)) &&
            (((w >> 10) & 3) != 3 || q || sequential)) return Structure;
    }
    if ((w & 0x9ff80c00) == 0x0f000400 &&
        (q || !((w >> 29) & 1) || ((w >> 12) & 15) != 15)) return Vector;
    unsigned immh = (w >> 19) & 15;
    if ((w & 0x9f800400) == 0x0f000400 && immh) {
        unsigned opcode = (w >> 11) & 31;
        bool unsigned_op = w & (1U << 29);
        // Narrow/widen forms use Q to select a half, including Q=0 32-bit lanes.
        if (opcode >= 16 && opcode <= 20 && !(immh & 8)) return Vector;
        if (q || !(immh & 8)) {
            switch (opcode) {
            case 0: case 2: case 4: case 6: case 10: case 14: return Vector;
            case 8: case 12: if (unsigned_op) return Vector; break;
            }
        }
    }
    if ((w & 0x9f20fc00) == 0x0e201c00) return Vector; // AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF
    switch (w & 0xbf20fc00) {
    case 0x0e001800: case 0x0e002800: case 0x0e003800:
    case 0x0e005800: case 0x0e006800: case 0x0e007800:
    case 0x0e208400: case 0x2e208400: case 0x0e20bc00:
    case 0x0e203400: case 0x0e203c00: case 0x2e203400:
    case 0x2e203c00: case 0x2e208c00: case 0x0e208c00:
    case 0x0e204400: case 0x2e204400: // SSHL/USHL
    case 0x0e204c00: case 0x2e204c00: // SQSHL/UQSHL
    case 0x0e205400: case 0x2e205400: // SRSHL/URSHL
        if (element != 3 || q) return Vector;
        break;
    case 0x0e209c00: case 0x0e209400: case 0x2e209400:
    case 0x0e206400: case 0x0e206c00: case 0x2e206400: case 0x2e206c00:
    case 0x0e20a400: case 0x0e20ac00: case 0x2e20a400: case 0x2e20ac00:
    case 0x0e20c000: case 0x2e20c000: // SMULL/UMULL
    case 0x0e208000: case 0x2e208000: // SMLAL/UMLAL
    case 0x0e20a000: case 0x2e20a000: // SMLSL/UMLSL
        if (element != 3) return Vector;
        break;
    }
    switch (w & 0xbffffc00) {
    case 0x0e205800: case 0x2e205800: case 0x2e605800: // CNT/NOT/RBIT
    case 0x0e201800: return Vector; // REV16
    }
    switch (w & 0xbf3ffc00) {
    case 0x0e209800: case 0x0e208800: case 0x2e208800:
    case 0x2e209800: case 0x0e20a800: // signed comparisons with zero
    case 0x0e20b800: case 0x2e20b800: // ABS/NEG
        if (element != 3 || q) return Vector;
        break;
    case 0x0e204800: case 0x2e204800: // CLS/CLZ
    case 0x0e200800: // REV64
        if (element != 3) return Vector;
        break;
    case 0x2e200800: if (element < 2) return Vector; break; // REV32
    case 0x0e303800: case 0x2e303800: // SADDLV/UADDLV
    case 0x0e30a800: case 0x2e30a800: // SMAXV/UMAXV
    case 0x0e31a800: case 0x2e31a800: case 0x0e31b800: // SMINV/UMINV/ADDV
        if (element < 2 || (element == 2 && q)) return Vector;
        break;
    case 0x0e202800: case 0x2e202800: // SADDLP/UADDLP
    case 0x0e206800: case 0x2e206800: // SADALP/UADALP
    case 0x0e212800: case 0x2e212800: // XTN/SQXTUN
    case 0x0e214800: case 0x2e214800: // SQXTN/UQXTN
    case 0x2e213800: // SHLL
        if (element < 3) return Vector;
        break;
    }
    if ((w & 0xbfe08400) == 0x2e000000 && (q || !(w & (1U << 14)))) return Vector;
    unsigned imm5 = (w >> 16) & 31;
    if (imm5) {
        unsigned lane_size = __builtin_ctz(imm5);
        if (lane_size < 4) {
            switch (w & 0xbfe0fc00) {
            case 0x0e000400: if (lane_size < 3 || q) return Vector; break;
            case 0x0e000c00: if (lane_size < 3 || q) return VectorFromGpr; break;
            case 0x0e002c00: if (lane_size < (q ? 3U : 2U)) return GprFromVector; break;
            case 0x0e003c00: if (q ? lane_size == 3 : lane_size < 3) return GprFromVector; break;
            }
            if ((w & 0xffe0fc00) == 0x4e001c00) return VectorFromGpr;
            if ((w & 0xffe08400) == 0x6e000400 &&
                !(((w >> 11) & 15) & ((1U << lane_size) - 1))) return Vector;
        }
    }
    // FP32/FP64 use the guest FPCR/FPSR. FP16 and newer optional features
    // retain QEMU's implementation and feature checks.
    if (!((w >> 22) & 1) || q) {
        switch (w & 0xbfa0fc00) {
        case 0x0e20cc00: case 0x0ea0cc00: case 0x0e20d400:
        case 0x0ea0d400: case 0x2e20dc00: case 0x2e20fc00: return Vector;
        }
    }
    if (element <= 1) {
        switch (w & 0xff3ffc00) {
        case 0x1e204000: case 0x1e20c000: case 0x1e214000:
        case 0x1e21c000: case 0x1e244000: case 0x1e24c000:
        case 0x1e254000: case 0x1e25c000: case 0x1e264000:
        case 0x1e274000: case 0x1e27c000: return Vector;
        }
        switch (w & 0xff20fc00) {
        case 0x1e200800: case 0x1e201800: case 0x1e202800:
        case 0x1e203800: case 0x1e204800: case 0x1e205800:
        case 0x1e206800: case 0x1e207800: return Vector;
        }
        if ((w & 0xff200c00) == 0x1e200c00 || // FCSEL
            (w & 0xff201fe0) == 0x1e201000 || // FMOV immediate
            (w & 0xff000000) == 0x1f000000) return Vector; // FMADD/FMSUB/FNMADD/FNMSUB
        if ((w & 0xff20fc07) == 0x1e202000 &&
            (!(w & 8) || !((w >> 16) & 31))) return Vector; // FCMP/FCMPE
        if ((w & 0x7f3ffc00) == 0x1e220000 ||
            (w & 0x7f3ffc00) == 0x1e230000) return FpFromGpr; // SCVTF/UCVTF
        if ((w & 0x7f3ffc00) == 0x1e380000 ||
            (w & 0x7f3ffc00) == 0x1e390000) return GprFromFp; // FCVTZS/FCVTZU
    }
    switch (w & 0xfffffc00) {
    case 0x1e270000: case 0x9e670000: case 0x9eaf0000: return FpFromGpr;
    case 0x1e260000: case 0x9e660000: case 0x9eae0000: return GprFromFp;
    case 0x1e22c000: case 0x1e624000: return Vector; // FCVT S/D
    }
    return Other;
}
bool integer_kind(Kind kind) { return kind >= AddImm && kind <= Carry; }
uint64_t target(uint32_t w, Kind kind, uint64_t pc)
{
    if (kind == Branch) return pc + sign_extend(w & 0x3ffffff, 26) * 4;
    unsigned bits = kind == TestBranch ? 14 : 19;
    return pc + sign_extend((w >> 5) & ((1U << bits) - 1), bits) * 4;
}
}

struct CompiledPage {
    struct Allocation { size_t offset, size; };
    uint64_t last_used = 0;
    std::vector<Allocation> allocations;
    uint64_t first_address = 0;
    uint64_t cache_key = 0;
    uint32_t compile_flags = 0;
    // Dormant code is not executable. Non-null code pointers store offset+1
    // until the page is restored into the executable arena.
    std::vector<uint32_t> dormant;
    std::vector<FaultSite> dormant_faults;
    std::list<CompiledPage *>::iterator dormant_position;
    struct Loop {
        size_t begin, end;
        unsigned base, extent = 0, alignment = 1;
        bool read = false, write = false;
        void *generic = nullptr, *fast = nullptr;
    };
    static constexpr size_t max_bytes = 1024 * 1024;
    std::array<uint32_t, 1024> source{};
    std::array<void *, 1024> entries{};
    std::array<void *, 1024> branch_entries{};
    std::vector<Loop> loops;
    bool unaligned = false;

    CompiledPage() = default;
    CompiledPage(const CompiledPage &other)
        : last_used(other.last_used), allocations(other.allocations), first_address(other.first_address),
          cache_key(other.cache_key), compile_flags(other.compile_flags), source(other.source),
          entries(other.entries), branch_entries(other.branch_entries),
          loops(other.loops), unaligned(other.unaligned) {}
    CompiledPage &operator=(CompiledPage &&) = default;

    size_t compile(uint64_t new_page, const void *bytes, size_t entry,
                   uint32_t *executable, uint32_t *writable, uint32_t flags,
                   std::vector<FaultSite> &fault_sites, size_t available, void **stable_entries)
    {
        std::vector<FaultSite> new_faults;
        bool landing = flags & CompileLanding;
        unaligned = flags & 1;
        std::array<uint32_t, 1024> input;
        std::memcpy(input.data(), bytes, 4096);
        auto mops_at = [&](size_t i) {
            return !landing && (flags & HUSK_MOPS_ALLOWED) && i + 2 < input.size() &&
                husk_arm64_mops_sequence(input.data() + i);
        };
        std::array<bool, 1024> emit{};
        std::vector<size_t> pending{entry};
        while (!pending.empty()) {
            size_t i = pending.back();
            pending.pop_back();
            for (; i < input.size() && !emit[i] && !entries[i]; ++i) {
                emit[i] = true;
                Kind k = decode(input[i]);
                if (mops_at(i)) {
                    if (i + 3 < input.size()) pending.push_back(i + 3);
                    break;
                }
                if (k >= Branch && k <= TestBranch) {
                    uint64_t address = target(input[i], k, new_page + i * 4);
                    if (address >= new_page && address - new_page < 4096)
                        pending.push_back((address - new_page) / 4);
                }
                if (landing || k == Other || k == Indirect ||
                    (k == Branch && !(input[i] & 0x80000000))) break;
            }
        }
        size_t first = 0, limit = input.size();
        while (!emit[first]) ++first;
        while (!emit[limit - 1]) --limit;
        first &= ~size_t(31);
        limit = (limit + 31) & ~size_t(31);
        std::vector<Label> labels(limit - first + 1), exhausted(limit - first);
        std::array<bool, 1025> leaders{};
        leaders[1024] = true;
        for (size_t i = first; i < limit; ++i) {
            if (!emit[i]) continue;
            if (!(i % 32) || i == entry || (i && !emit[i - 1])) leaders[i] = true;
            Kind k = decode(input[i]);
            if (k >= Branch && k <= TestBranch) {
                uint64_t address = target(input[i], k, new_page + i * 4);
                if (address >= new_page && address - new_page < 4096)
                    leaders[(address - new_page) / 4] = true;
            }
            if (k == Other || (k >= Branch && k <= Indirect)) leaders[i + 1] = true;
        }
        size_t old_loops = loops.size();
        const char *loop_setting = std::getenv("HUSK_ARM64_LOOP_MAP");
        if (!landing && (!loop_setting || std::strcmp(loop_setting, "0"))) {
            for (size_t end = std::max(size_t(1), first); end < limit; ++end) {
                if (!emit[end] || decode(input[end]) != Conditional) continue;
                uint64_t start = target(input[end], Conditional, new_page + end * 4);
                if (start < new_page || start >= new_page + end * 4) continue;
                size_t begin = (start - new_page) / 4;
                if (end - begin >= 32) continue;
                Loop loop{begin, end, 32};
                bool valid = true;
                for (size_t j = begin; j < end; ++j) {
                    if (!emit[j]) valid = false;
                    auto w = input[j];
                    auto kind = decode(w);
                    if (kind == Memory && (w & 0x3f000000) == 0x39000000 && ((w >> 22) & 3) <= 1) {
                        unsigned base = (w >> 5) & 31, size = 1U << (w >> 30);
                        if (base == 31 || (loop.base != 32 && loop.base != base)) valid = false;
                        loop.base = base;
                        loop.extent = std::max(loop.extent, (((w >> 10) & 4095) + 1) * size);
                        loop.alignment = std::max(loop.alignment, size);
                        (w & (1U << 22) ? loop.read : loop.write) = true;
                    } else if (!integer_kind(kind) || (kind == Crc && !(flags & 64))) {
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
        auto relative_address = [&](XReg reg, uint64_t offset, XReg scratch) {
            c.LDR(reg, X27, offsetof(Frame, code_page));
            if (!offset) return;
            if (offset <= 4095) c.ADD(reg, reg, offset);
            else if (offset >= UINT64_MAX - 4094) c.SUB(reg, reg, -offset);
            else { c.MOV(scratch, offset); c.ADD(reg, reg, scratch); }
        };
        auto store_pc = [&](uint64_t pc) {
            relative_address(X17, pc - new_page, X16); c.STR(X17, X27, HUSK_PC);
        };
        auto leave = [&](Stop reason) {
            c.MOV(X17, uint64_t(reason)); c.STR(X17, X27, HUSK_REASON);
            c.MOV(X16, uint64_t(&husk_direct_leave)); c.BR(X16);
        };
        auto exit = [&](uint64_t pc, Stop reason) {
            if ((flags & CompileSharedStubs) && pc - new_page < 4096) {
                // A page-relative PC keeps shared guest images relocatable.
                c.MOV(W17, uint32_t(((pc - new_page) << 4) | reason));
                c.LDR(X26, X27, HUSK_EXIT_STUB); c.BR(X26);
            } else { store_pc(pc); leave(reason); }
        };
        auto load = [&](unsigned r, XReg to, bool sp = false) {
            if (resident(r)) c.MOV(to, XReg(r));
            else if (r == 31 && !sp) c.MOV(to, 0);
            else c.LDR(to, X27, r * 8);
        };
        auto save = [&](unsigned r, XReg from, bool sp = false) {
            if (resident(r)) c.MOV(XReg(r), from);
            else if (r != 31 || sp) c.STR(from, X27, r * 8);
        };
        // X16 holds a guest target. The owner and guest page travel with a
        // cached destination, allowing identical code at different guest VAs.
        auto dynamic_jump = [&] {
            if (flags & CompileSharedStubs) {
                c.LDR(X26, X27, HUSK_BRANCH_STUB); c.BR(X26);
                return;
            }
            Label miss;
            c.AND(X17, X16, 3); c.CBNZ(X17, miss);
            c.LDR(X26, X27, HUSK_CODE_ROUTES);
            c.UBFX(X17, X16, 12, 8); c.ADD(X17, X26, X17, LSL, 5);
            c.LDR(X26, X17); c.EOR(X26, X26, X16, LSR, 12); c.CBNZ(X26, miss);
            c.LDR(X26, X17, 8);
            c.AND(X30, X16, 4095); c.ADD(X26, X26, X30, LSL, 1);
            c.LDR(X26, X26); c.CBZ(X26, miss);
            c.LDR(X17, X17, 16); c.STR(X17, X27, offsetof(Frame, code));
            c.LDR(X30, X27, HUSK_CACHE_CLOCK); c.STR(X30, X17, HUSK_CODE_LAST_USED);
            c.AND(X17, X16, ~UINT64_C(4095)); c.STR(X17, X27, offsetof(Frame, code_page));
            c.BR(X26);
            c.l(miss); c.STR(X16, X27, HUSK_PC); leave(CodeBranch);
        };
        auto jump = [&](uint64_t pc) {
            // Landing variants only own the first instruction. Continuations
            // must use normal code so memory retries select the matching page.
            if (!landing && pc >= new_page && pc - new_page < 4096) {
                size_t index = (pc - new_page) / 4;
                if (emit[index]) { c.B(labels[index - first]); return; }
                if (branch_entries[index]) {
                    if (flags & CompileRelocatable) {
                        c.MOV(X16, uint64_t(stable_entries));
                        c.LDR(X16, X16, index * sizeof(void *));
                    } else {
                        c.MOV(X16, uint64_t(branch_entries[index]));
                    }
                    c.BR(X16); return;
                }
            }
            relative_address(X16, pc - new_page, X17);
            dynamic_jump();
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
                               kind == Binary || kind == Crc || kind == Select || kind == Multiply ||
                               kind == AddExt || kind == Carry;
            unsigned n = 0, m = 0, a = 0;
            if (kind != MoveWide) n = operand(rn, kind == AddImm || kind == AddExt);
            if (two_sources) m = operand(rm);
            if (kind == Multiply) a = operand(ra);
            bool dest_sp = ((kind == AddImm || kind == AddExt) && !set_flags) ||
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
        auto mops = [&](uint32_t w, uint64_t pc) {
            unsigned rd = w & 31, rn = (w >> 5) & 31, rs = (w >> 16) & 31;
            bool set = (w & 0xffe0fc00) == 0x19c00400;
            Label slow, read_miss, write_miss, copied;
            load(rn, X16);
            c.SUB(X17, X16, 1); c.LSR(X17, X17, 6); c.CBNZ(X17, slow);
            auto map = [&](unsigned reg, XReg host, bool write, Label &miss) {
                load(reg, X16);
                if (flags & 8) c.AND(X16, X16, UINT64_C(0x00ffffffffffffff));
                c.LSR(X17, X16, 48); c.CBNZ(X17, slow);
                load(rn, X17); c.ADD(X17, X16, X17); c.SUB(X17, X17, 1);
                c.EOR(X17, X17, X16); c.LSR(X17, X17, 12); c.CBNZ(X17, slow);
                c.LSR(X17, X16, 12); c.EOR(X17, X17, X17, LSR, 8); c.AND(X17, X17, 255);
                c.ADD(X17, X28, X17, LSL, 5);
                c.LDP(host, X17, X17, write ? 16 : 0);
                c.EOR(host, host, X16, LSR, 12); c.CBNZ(host, miss);
                c.ADD(host, X16, X17);
            };
            map(rd, X26, true, write_miss);
            if (!set) map(rs, X30, false, read_miss);

            // All memory is validated before borrowing guest SIMD registers.
            // First/last chunks are loaded before any store, including overlap.
            c.STP(Q28, Q29, X27, HUSK_VECTORS + 28 * 16);
            c.STP(Q30, Q31, X27, HUSK_VECTORS + 30 * 16);
            if (set) {
                load(rs, X16);
                c.dw(0x4e010c00 | (16 << 5) | 28); // dup v28.16b,w16
                c.MOV(V29.B16(), V28.B16());
            }
            load(rn, X16);
            std::array<Label, 6> sizes;
            for (int bit = 5; bit >= 1; --bit) {
                c.LSR(X17, X16, bit); c.CBNZ(X17, sizes[bit]);
            }
            c.B(sizes[0]);
            auto chunks = [&](auto first, auto last, unsigned size, unsigned bit) {
                c.l(sizes[bit]);
                c.SUB(X16, X16, size);
                if (!set) {
                    c.ADD(X17, X30, X16);
                    c.LDR(first, X30); c.LDR(last, X17);
                }
                c.ADD(X17, X26, X16);
                c.STR(first, X26); c.STR(set ? first : last, X17);
                c.B(copied);
            };
            chunks(B28, B29, 1, 0);
            chunks(H28, H29, 2, 1);
            chunks(S28, S29, 4, 2);
            chunks(D28, D29, 8, 3);
            chunks(Q28, Q29, 16, 4);
            c.l(sizes[5]);
            c.SUB(X16, X16, 32);
            if (!set) {
                c.ADD(X17, X30, X16);
                c.LDP(Q28, Q29, X30); c.LDP(Q30, Q31, X17);
            }
            c.ADD(X17, X26, X16);
            c.STP(Q28, Q29, X26);
            if (set) c.STP(Q28, Q29, X17); else c.STP(Q30, Q31, X17);
            c.l(copied);
            c.LDP(Q28, Q29, X27, HUSK_VECTORS + 28 * 16);
            c.LDP(Q30, Q31, X27, HUSK_VECTORS + 30 * 16);
            load(rn, X30);
            c.LSR(X17, X30, 4); c.SUB(X29, X29, X17);
            load(rd, X16);
            if (!set) {
                // Option A leaves pointers at their starting addresses after
                // reverse copies; all other completed sequences advance them.
                load(rs, X17);
                if (flags & 8) {
                    c.AND(X26, X16, UINT64_C(0x00ffffffffffffff));
                    c.AND(X17, X17, UINT64_C(0x00ffffffffffffff));
                    c.SUB(X26, X26, X17);
                } else c.SUB(X26, X16, X17);
                c.SUB(X26, X26, 1);
                c.SUB(X17, X30, 1); c.CMP(X26, X17); c.CSEL(X30, XZR, X30, Cond::LO);
                load(rs, X17); c.ADD(X17, X17, X30); save(rs, X17);
            }
            c.ADD(X16, X16, X30); save(rd, X16); save(rn, XZR);
            c.MSR(SystemReg::NZCV, XZR);
            jump(pc + 12);
            c.l(read_miss); c.STR(X16, X27, HUSK_ADDRESS); exit(pc, ReadPage);
            c.l(write_miss); c.STR(X16, X27, HUSK_ADDRESS); exit(pc, WritePage);
            c.l(slow); exit(pc, Fallback);
        };

        for (size_t i = first; i < limit; ++i) {
            if (emit[i]) {
                uint64_t pc = new_page + i * 4;
                c.l(labels[i - first]);
                branch_entries[i] = c.xptr<void *>();
                if (leaders[i]) {
                    size_t end = i + 1;
                    while (!leaders[end] && emit[end]) ++end;
                    c.SUB(X29, X29, end - i);
                    c.TBNZ(X29, 63, exhausted[i - first]);
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
                bool fp_access = kind == Structure || (kind >= Vector && kind <= GprFromFp) ||
                    ((kind == Memory || kind == Pair) && (w & 0x04000000)) ||
                    (kind == SystemRead && ((w & 0xffffffe0) == 0xd53b4400 ||
                                           (w & 0xffffffe0) == 0xd53b4420));
                if ((flags & 2) && fp_access) kind = Other;
                if (kind == Atomic && !(flags & 4)) kind = Other;
                if (kind == Crc && !(flags & 64)) kind = Other;
                if ((kind == CryptoAes && !(flags & CompileAes)) ||
                    (kind == CryptoSha1 && !(flags & CompileSha1)) ||
                    (kind == CryptoSha256 && !(flags & CompileSha256))) kind = Other;
                if ((kind == ExclusiveLoad || kind == ExclusiveStore || kind == ClearExclusive) &&
                    !(flags & CompileExclusive)) kind = Other;
                if (mops_at(i)) {
                    mops(w, pc);
                } else if (integer_kind(kind)) {
                    integer(w, kind, 26);
                } else if (kind == Pauth && (flags & CompilePauthNop)) {
                    // Only the architectural HINT encodings become NOPs.
                } else if (kind == Pauth && (flags & CompileInlineAssists)) {
                    Label slow, done;
                    unsigned reg = pauth_register(w);
                    load(reg, X16);
                    if (reg == 17) load(16, X17);
                    else if (w != 0xd50320ff && (w & 32)) load(31, X17, true);
                    else c.MOV(X17, 0);
                    c.MOV(W26, w);
                    c.MOV(X30, uint64_t(&husk_direct_pauth)); c.BLR(X30);
                    c.CBZ(X26, slow);
                    if (flags & CompilePauthVerify) {
                        Label cached;
                        c.SUB(X26, X26, 1); c.CBNZ(X26, cached);
                        c.STR(X16, X27, HUSK_ADDRESS); exit(pc, PauthCheck);
                        c.l(cached);
                    }
                    save(reg, X16); c.B(done);
                    c.l(slow); exit(pc, Fallback);
                    c.l(done);
                } else if (kind == Barrier && (flags & CompileInlineAssists)) {
                    // QEMU's assist uses a full host memory barrier. Device
                    // accesses leave Direct before reaching this instruction.
                    c.dw(0xd5033bbf); // dmb ish
                } else if (kind == Bti && (flags & CompileInlineAssists)) {
                    // The owner checks guarded targets before entering Direct.
                } else if (kind == ClearExclusive) {
                    c.MOV(X16, UINT64_MAX); c.STR(X16, X27, HUSK_EXCLUSIVE_ADDR);
                } else if (kind == Hint) {
                    // The guest NOP has no architectural side effects.
                } else if (kind == KernelRead) {
                    if (!(flags & CompileKernelReads)) exit(pc, Fallback);
                    else {
                        unsigned offset = (w & 0xffffffe0) == 0xd538d080 ? 0 :
                            (w & 0xffffffe0) == 0xd5384100 ? 8 : 16;
                        c.LDR(X16, X27, HUSK_KERNEL_READS);
                        c.LDR(X16, X16, offset); save(rd, X16);
                    }
                } else if (kind == SystemRead) {
                    if ((w & 0xffffffe0) == 0xd53bd040) c.LDR(X16, X27, HUSK_TPIDR);
                    else c.dw((w & ~31U) | 16);
                    save(rd, X16);
                } else if (kind == Compare) {
                    load(rn, X16);
                    w = (w & ~(31U << 5)) | (16 << 5);
                    if (!(w & (1U << 11))) {
                        load((w >> 16) & 31, X17);
                        w = (w & ~(31U << 16)) | (17 << 16);
                    }
                    c.dw(w);
                } else if (kind == Vector || kind == CryptoAes || kind == CryptoSha1 || kind == CryptoSha256) {
                    c.dw(w);
                } else if (kind == VectorFromGpr || kind == FpFromGpr) {
                    load(rn, X16);
                    c.dw((w & ~(31U << 5)) | (16 << 5));
                } else if (kind == GprFromVector || kind == GprFromFp) {
                    c.dw((w & ~31U) | 16);
                    save(rd, X16);
                } else if (kind == Adr) {
                    int64_t imm = sign_extend(((w >> 29) & 3) | ((w >> 3) & 0x1ffffc), 21);
                    uint64_t value = w >> 31 ? (pc & ~UINT64_C(4095)) + imm * 4096 : pc + imm;
                    relative_address(X16, value - new_page, X17); save(rd, X16);
                } else if (kind >= Branch && kind <= TestBranch) {
                    uint64_t dest = target(w, kind, pc);
                    if (kind == Branch) {
                        if (w >> 31) { relative_address(X16, pc + 4 - new_page, X17); save(30, X16); }
                        jump(dest);
                    } else {
                        Label outside, next;
                        bool in_page = dest >= new_page && dest - new_page < 4096;
                        bool near_branch = in_page && emit[(dest - new_page) / 4] && kind != TestBranch;
                        Label &taken = near_branch ? labels[(dest - new_page) / 4 - first] : outside;
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
                } else if ((w & 0xfffffc1f) == 0xd63f0000 ||
                           ((flags & CompileTail) && (w & 0xfffffc1f) == 0xd61f0000 && rn != 31)) {
                    bool link = (w & 0xfffffc1f) == 0xd63f0000;
                    Label miss, exhausted;
                    load(rn, X16);
                    c.LDR(X26, X27, HUSK_CALL_ROUTES);
                    if (!link) c.ADD(X26, X26, 16384);
                    c.EOR(X17, X16, X16, LSR, 8);
                    c.UBFX(X17, X17, 2, 8); c.ADD(X26, X26, X17, LSL, 6);
                    c.LDR(X17, X26); c.EOR(X17, X17, X16); c.CBNZ(X17, miss);
                    if (!link) {
                        // General BR obtains BTYPE from its source page. Keep
                        // that page and the x16/x17 distinction in the key.
                        if (rn == 16 || rn == 17) c.MOV(X17, 1);
                        else {
                            c.LDR(X17, X27, offsetof(Frame, code_page));
                            c.ORR(X17, X17, 3);
                        }
                        c.LDR(X30, X26, offsetof(CallRoute, source_tag));
                        c.EOR(X17, X17, X30); c.CBNZ(X17, miss);
                    }
                    c.LDR(X30, X26, 8); c.CBZ(X30, miss);
                    c.SUB(X29, X29, 1); c.TBNZ(X29, 63, exhausted);
                    c.LDR(W17, X27, HUSK_ACCESS_FLAGS);
                    c.AND(W17, W17, ~uint32_t(HUSK_BTYPE_MASK));
                    c.LDR(W16, X26, offsetof(CallRoute, btype));
                    c.ORR(W17, W17, W16); c.STR(W17, X27, HUSK_ACCESS_FLAGS);
                    if (link) { relative_address(X17, pc + 4 - new_page, X16); save(30, X17); }
                    c.LDR(X17, X26, 16); c.STR(X17, X27, offsetof(Frame, code_page));
                    c.LDR(X17, X26, 24); c.STR(X17, X27, offsetof(Frame, code));
                    c.LDR(X16, X27, HUSK_CACHE_CLOCK); c.STR(X16, X17, HUSK_CODE_LAST_USED);
                    c.BR(X30);
                    c.l(miss); exit(pc, Fallback);
                    c.l(exhausted); exit(pc, Budget);
                } else if (kind == Indirect) {
                    Label budget_end;
                    // A dynamic target can be inside a static block. Charge each
                    // return too, so a self-return cannot bypass the slice budget.
                    c.SUB(X29, X29, 1); c.TBNZ(X29, 63, budget_end);
                    load(rn, X16);
                    Label outside;
                    c.LDR(X17, X27, offsetof(Frame, code_page));
                    c.EOR(X17, X17, X16); c.LSR(X17, X17, 12); c.CBNZ(X17, outside);
                    c.AND(X17, X16, 3); c.CBNZ(X17, outside);
                    // This immutable page already has execute permission. Local
                    // returns use the same budget-checked entries as direct branches.
                    c.AND(X26, X16, 4095);
                    c.MOV(X17, uint64_t(stable_entries));
                    c.ADD(X17, X17, X26, LSL, 1);
                    c.LDR(X26, X17); c.CBZ(X26, outside); c.BR(X26);
                    c.l(outside);
                    dynamic_jump();
                    c.l(budget_end); exit(pc, Budget);
                } else if (kind == ExclusiveLoad || kind == ExclusiveStore) {
                    bool write = kind == ExclusiveStore;
                    unsigned size = w >> 30, status = (w >> 16) & 31;
                    Label failed, miss, invalid, done, completed;
                    load(rn, X16, true);
                    if (flags & 8) c.TBNZ(X16, 55, invalid);
                    auto clean = [&](XReg to) {
                        if (flags & 8) {
                            c.AND(to, X16, UINT64_C(0x00ffffffffffffff));
                        } else c.MOV(to, X16);
                    };
                    if (write) {
                        if (w & 0x8000) c.dw(0xd5033bbf); // release also orders a failed STLXR
                        clean(X26); c.LDR(X17, X27, HUSK_EXCLUSIVE_ADDR);
                        c.EOR(X17, X17, X26); c.CBNZ(X17, failed);
                    }
                    // QEMU checks the monitor before store alignment or access.
                    if (rn == 31) { c.AND(X17, X16, 15); c.CBNZ(X17, invalid); }
                    if (size) { c.AND(X17, X16, (1U << size) - 1); c.CBNZ(X17, invalid); }
                    // A software monitor survives mapping callbacks and a TCG
                    // transition. Host exclusive reservations would not.
                    c.LSR(X17, X16, 12); c.EOR(X17, X17, X17, LSR, 8); c.AND(X17, X17, 255);
                    c.ADD(X17, X28, X17, LSL, 5);
                    c.LDP(X26, X17, X17, write ? 16 : 0);
                    c.EOR(X26, X26, X16, LSR, 12); c.CBNZ(X26, miss);
                    c.ADD(X17, X16, X17);
                    if (write) {
                        c.LDR(X26, X27, HUSK_EXCLUSIVE_VALUE); load(rd, X30);
                        c.dw((size << 30) | 0x08e0fc00 | (26 << 16) | (17 << 5) | 30); // casal
                        c.LDR(X17, X27, HUSK_EXCLUSIVE_VALUE);
                        c.EOR(X17, X17, X26); c.CBNZ(X17, failed);
                        c.MOV(X26, 0); c.B(completed);
                        c.l(failed); c.MOV(X26, 1);
                        c.l(completed); save(status, X26);
                        c.MOV(X26, UINT64_MAX); c.STR(X26, X27, HUSK_EXCLUSIVE_ADDR);
                    } else {
                        c.dw((size << 30) | 0x08dffc00 | (17 << 5) | 26); // ldar, stronger than relaxed LDXR
                        c.STR(X26, X27, HUSK_EXCLUSIVE_VALUE);
                        clean(X17); c.STR(X17, X27, HUSK_EXCLUSIVE_ADDR); save(rd, X26);
                    }
                    c.B(done);
                    c.l(miss); c.STR(X16, X27, HUSK_ADDRESS); exit(pc, write ? WritePage : ReadPage);
                    c.l(invalid); exit(pc, Fallback);
                    c.l(done);
                } else if (kind == Memory || kind == Pair || kind == Atomic || kind == Structure) {
                    bool pair = kind == Pair;
                    bool atomic = kind == Atomic;
                    bool structure = kind == Structure;
                    bool ordered = atomic || (!pair && !structure && (w & 0x3fbffc00) == 0x089ffc00);
                    bool vector = w & 0x04000000;
                    bool write = atomic || (pair || vector || ordered ? !(w & (1U << 22)) : ((w >> 22) & 3) == 0);
                    bool reg_offset = !structure && !pair && (w & 0x3b200c00) == 0x38200800;
                    bool indexed = !structure && !pair && !ordered && !reg_offset && (w & 0x3b000000) != 0x39000000;
                    unsigned mode = structure ? ((w >> 23) & 1) : pair ? (((w >> 23) & 1) ? ((w >> 23) & 3) : 0) :
                                    indexed ? (w >> 10) & 3 : 0;
                    unsigned size_log2 = structure ? (w >> 10) & 3 : pair ? 2 + (vector ? w >> 30 : w >> 31) :
                                               (w >> 30) + (vector ? ((w >> 23) & 1) * 4 : 0);
                    unsigned access_bytes = structure ? structure_registers(w) * (w & (1U << 30) ? 16 : 8) :
                                                       (pair ? 2U : 1U) << size_log2;
                    int64_t offset = structure ? 0 : pair ? sign_extend((w >> 15) & 127, 7) * (1 << size_log2) :
                                     reg_offset || ordered ? 0 : indexed ? sign_extend((w >> 12) & 511, 9) :
                                     ((w >> 10) & 4095) << size_log2;
                    Label miss, invalid, done, checked, access;
                    FaultSite fault{};
                    load(rn, X16, true);
                    if (rn == 31) { c.AND(X17, X16, 15); c.CBNZ(X17, invalid); }
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
                    // Retain tags in both the mapping key and architectural
                    // writeback. The owner's MMU validates them before mapping RAM.
                    if (size_log2 && (!unaligned || ordered)) {
                        c.AND(X17, X16, (1U << size_log2) - 1); c.CBNZ(X17, invalid);
                    }
                    if (structure || pair || (unaligned && !ordered && size_log2)) {
                        c.ADD(X17, X16, access_bytes - 1);
                        c.EOR(X17, X17, X16); c.LSR(X17, X17, 12); c.CBNZ(X17, invalid);
                    }
                    if (flags & CompileFastmem) {
                        fault.patch = uintptr_t(c.xptr<void *>());
                        c.NOP(); // Replaced with a branch if this site needs checked mappings.
                        if (flags & 8) c.UBFX(X17, X16, HuskArm64Fastmem::address_bits,
                                             56 - HuskArm64Fastmem::address_bits);
                        else {
                            Label lower;
                            c.LSR(X17, X16, HuskArm64Fastmem::address_bits);
                            c.CBZ(X17, lower); c.EOR(X17, X17, 65535);
                            c.l(lower);
                        }
                        c.CBNZ(X17, checked);
                        c.LDR(X26, X27, write ? HUSK_WRITE_WINDOWS : HUSK_READ_WINDOWS);
                        c.UBFX(X17, X16, HuskArm64Fastmem::segment_bits,
                               HuskArm64Fastmem::address_bits - HuskArm64Fastmem::segment_bits);
                        if (!(flags & 8)) {
                            c.LSR(X30, X16, 63);
                            c.ORR(X17, X17, X30, LSL,
                                  HuskArm64Fastmem::address_bits - HuskArm64Fastmem::segment_bits);
                        }
                        c.LDR(X17, X26, X17, LSL, 3);
                        c.AND(X26, X16, HuskArm64Fastmem::segment_size - 1);
                        c.ADD(X17, X17, X26);
                        c.B(access);
                    }
                    c.l(checked);
                    fault.checked = uintptr_t(c.xptr<void *>());
                    c.LSR(X17, X16, 12); c.EOR(X17, X17, X17, LSR, 8); c.AND(X17, X17, 255);
                    c.ADD(X17, X28, X17, LSL, 5);
                    c.LDP(X26, X17, X17, write ? 16 : 0);
                    c.EOR(X26, X26, X16, LSR, 12); c.CBNZ(X26, miss);
                    c.ADD(X17, X16, X17);
                    c.l(access);
                    unsigned data = vector || resident(rd) || rd == 31 ? rd : 26;
                    if (structure) {
                        // Validate the complete list before issuing the host access.
                        // Guest writeback follows success and retains address tags.
                        fault.instruction = uintptr_t(c.xptr<void *>());
                        c.dw((w & ~((1U << 23) | (31U << 16) | (31U << 5))) | (17 << 5));
                    } else if (atomic) {
                        unsigned rs = (w >> 16) & 31;
                        load(rs, X26); load(rd, X30);
                        fault.instruction = uintptr_t(c.xptr<void *>());
                        c.dw((w & ~((31U << 16) | 1023U)) | (26 << 16) | (17 << 5) | 30);
                        if ((w & 0x3fa07c00) == 0x08a07c00) save(rs, X26);
                        else save(rd, X30);
                    } else if (pair) {
                        if (write && !vector && data == 26) load(rd, X26);
                        unsigned rt2 = (w >> 10) & 31;
                        unsigned data2 = vector || resident(rt2) || rt2 == 31 ? rt2 : 30;
                        if (write && !vector && data2 == 30) load(rt2, X30);
                        // Preserve a single architectural pair access after validating
                        // both elements, including their common page boundary.
                        fault.instruction = uintptr_t(c.xptr<void *>());
                        c.dw((w & 0xc4400000) | 0x29000000 | (data2 << 10) | (17 << 5) | data);
                        if (!write && !vector) {
                            if (data == 26) save(rd, X26);
                            if (data2 == 30) save(rt2, X30);
                        }
                    } else {
                        if (write && !vector && data == 26) load(rd, X26);
                        fault.instruction = uintptr_t(c.xptr<void *>());
                        c.dw(ordered ? (w & ~1023U) | (17 << 5) | data :
                             (w & 0xc4c00000) | 0x39000000 | (17 << 5) | data);
                        if (!write && !vector && data == 26) save(rd, X26);
                    }
                    if (mode) {
                        if (structure) {
                            unsigned rm = (w >> 16) & 31;
                            if (rm == 31) c.ADD(X16, X16, access_bytes);
                            else { load(rm, X17); c.ADD(X16, X16, X17); }
                        } else if (mode == 1 && offset) {
                            if (offset > 0) c.ADD(X16, X16, offset); else c.SUB(X16, X16, -offset);
                        }
                        save(rn, X16, true);
                    }
                    c.B(done);
                    if (flags & CompileFastmem) {
                        fault.slow = uintptr_t(c.xptr<void *>());
                        fault.bytes = access_bytes;
                        fault.write = write;
                        new_faults.push_back(fault);
                        c.STR(X16, X27, HUSK_ADDRESS);
                        exit(pc, write ? FastWritePage : FastReadPage);
                    }
                    c.l(miss);
                    c.STR(X16, X27, HUSK_ADDRESS);
                    exit(pc, write ? WritePage : ReadPage);
                    c.l(invalid); exit(pc, Fallback);
                    c.l(done);
                } else {
                    exit(pc, Fallback);
                }
                if (landing) {
                    c.LDR(W17, X27, HUSK_ACCESS_FLAGS);
                    c.AND(W17, W17, ~uint32_t(HUSK_BTYPE_MASK)); c.STR(W17, X27, HUSK_ACCESS_FLAGS);
                    jump(pc + 4);
                } else if (!mops_at(i) && i + 1 < input.size() && !emit[i + 1]) jump(pc + 4);
            }
            if (i % 32 == 31) {
                // TBNZ has a 32 KiB reach. Keep budget exits near their callers.
                Label next;
                c.B(next);
                for (size_t j = i - 31; j <= i; ++j) {
                    if (emit[j] && leaders[j]) { c.l(exhausted[j - first]); exit(new_page + j * 4, Budget); }
                }
                c.l(next);
            }
        }
        c.l(labels[limit - first]);
        exit(new_page + 4096, CodeBranch);
        for (size_t index = old_loops; index < loops.size(); ++index) {
            auto &loop = loops[index];
            loop.fast = c.xptr<void *>();
            c.LDR(X26, X27, HUSK_LOOP_BASE);
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
        // The caller compiles into a private metadata copy. A fragmented range
        // can be too small; report the required size without publishing anything.
        if (((output.size() * 4 + 15) & ~size_t(15)) > available) return output.size() * 4;
#if !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(0);
#endif
        std::memcpy(writable, output.data(), output.size() * 4);
#if !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(1);
#endif
        sys_icache_invalidate(executable, output.size() * 4);
        auto old_faults = fault_sites.size();
        fault_sites.insert(fault_sites.end(), new_faults.begin(), new_faults.end());
        // Reclaimed allocations can precede live code. The signal handler uses
        // binary search, so publishing a new block must preserve address order.
        std::inplace_merge(fault_sites.begin(), fault_sites.begin() + old_faults, fault_sites.end(),
                          [](const auto &a, const auto &b) { return a.instruction < b.instruction; });
        source = input;
        return output.size() * 4;
    }
};

static_assert(offsetof(CompiledPage, last_used) == HUSK_CODE_LAST_USED);

struct HuskArm64Direct {
    const size_t capacity;
    uint32_t *executable;
    uint32_t *writable;
#if TARGET_OS_IPHONE
    HuskDualMapping mapping{};
#endif
    size_t used = 0;
    std::map<size_t, size_t> free_ranges;
    std::set<std::pair<size_t, size_t>> free_sizes;
    std::unordered_multimap<uint64_t, std::unique_ptr<CompiledPage>> pages;
    // Native route hits update last_used directly. Heap timestamps are lower
    // bounds; refresh stale roots before choosing a page to evict.
    using CodeAge = std::pair<uint64_t, CompiledPage *>;
    std::vector<CodeAge> code_age;
    std::list<CompiledPage *> dormant_pages;
    size_t dormant_limit = 0, dormant_bytes = 0;
    uint64_t restores = 0;
    std::array<Mapping, 256> mappings{};
    std::array<PauthResult, 256> pauth_cache{};
    std::array<CodeRoute, 256> routes{};
    // BLR and BR use separate banks. BR also validates the source page/class.
    std::array<CallRoute, 512> call_routes{};
    std::array<SourceRoute, 256> source_routes{};
    std::array<uint64_t, 4> populated{};
    std::unique_ptr<HuskArm64Fastmem> fastmem;
    std::vector<FaultSite> fault_sites;
    uint64_t mapping_epoch = 0;
    void *mapping_owner = nullptr;
    decltype(HuskArm64Memory::map_page) mapping_callback = nullptr;
    bool profile = std::getenv("HUSK_ARM64_STATS") != nullptr;
    bool native_lse = false, native_crc = false;
    uint32_t native_crypto = 0;
    bool verify_pauth = std::getenv("HUSK_ARM64_NATIVE_PAUTH_VERIFY") != nullptr;
    uint64_t pauth_checks = 0;
    bool inline_assists = true;
    bool exclusive = true;
    bool tail_cache = true;
    bool reclaim_cache = true;
    bool share_code = true;
    bool shared_stubs = true;
    uint64_t shared_hits = 0;
    uint64_t cache_clock = 0, evictions = 0;
    uint64_t code_version_hits = 0, code_checks = 0;
    void *version_owner = nullptr;
    decltype(HuskArm64Memory::code_version) version_callback = nullptr;
    uint64_t calls = 0, compiles = 0, flushes = 0, compile_ns = 0, run_ns = 0;
    uint64_t branches = 0, budget_exits = 0, assists = 0, chained_assists = 0;
    std::unordered_map<uint32_t, uint64_t> fallbacks, assist_samples;
    std::chrono::steady_clock::time_point report = std::chrono::steady_clock::now();

    explicit HuskArm64Direct(size_t cache_bytes) : capacity(cache_bytes)
    {
        if (capacity < 2 * CompiledPage::max_bytes || capacity > 256 * 1024 * 1024 || capacity % 16384)
            throw std::invalid_argument("Direct cache must be 2..256 MiB, aligned to 16 KiB");
        release_code({0, capacity});
        const char *reclaim_setting = std::getenv("HUSK_ARM64_CACHE_RECLAIM");
        reclaim_cache = !reclaim_setting || std::strcmp(reclaim_setting, "0");
        if (const char *setting = std::getenv("HUSK_ARM64_COLD_CACHE_MIB")) {
            char *end;
            errno = 0;
            unsigned long mib = std::strtoul(setting, &end, 10);
            if (errno || end == setting || *end || mib > 256)
                throw std::invalid_argument("HUSK_ARM64_COLD_CACHE_MIB must be 0..256");
            if (reclaim_cache) dormant_limit = mib * 1024 * 1024;
        }
        const char *sharing_setting = std::getenv("HUSK_ARM64_CODE_SHARING");
        share_code = !sharing_setting || std::strcmp(sharing_setting, "0");
        const char *stub_setting = std::getenv("HUSK_ARM64_SHARED_STUBS");
        shared_stubs = !stub_setting || std::strcmp(stub_setting, "0");
        const char *inline_setting = std::getenv("HUSK_ARM64_INLINE_ASSISTS");
        inline_assists = !inline_setting || std::strcmp(inline_setting, "0");
        const char *exclusive_setting = std::getenv("HUSK_ARM64_EXCLUSIVE");
        exclusive = !exclusive_setting || std::strcmp(exclusive_setting, "0");
        const char *tail_setting = std::getenv("HUSK_ARM64_TAIL_CACHE");
        tail_cache = !tail_setting || std::strcmp(tail_setting, "0");
        const char *fast = std::getenv("HUSK_ARM64_FASTMEM");
        if (fast && !std::strcmp(fast, "1")) {
            fastmem = std::make_unique<HuskArm64Fastmem>();
            if (fastmem->available()) {
                install_fastmem_signals();
                std::fprintf(stderr, "[husk-fastmem] segmented mapping active\n");
            } else {
                fastmem.reset();
                std::fprintf(stderr, "[husk-fastmem] reservation unavailable; checked mappings active\n");
            }
        }
        mappings.fill(EmptyMapping);
        int lse = 0;
        size_t lse_size = sizeof(lse);
        native_lse = !sysctlbyname("hw.optional.arm.FEAT_LSE", &lse, &lse_size, nullptr, 0) && lse;
        int crc = 0;
        size_t crc_size = sizeof(crc);
        native_crc = !sysctlbyname("hw.optional.armv8_crc32", &crc, &crc_size, nullptr, 0) && crc;
        const char *crypto_setting = std::getenv("HUSK_ARM64_NATIVE_CRYPTO");
        if (!crypto_setting || std::strcmp(crypto_setting, "0")) {
            for (auto feature : {std::pair{"hw.optional.arm.FEAT_AES", CompileAes},
                                 std::pair{"hw.optional.arm.FEAT_SHA1", CompileSha1},
                                 std::pair{"hw.optional.arm.FEAT_SHA256", CompileSha256}}) {
                int supported = 0;
                size_t size = sizeof(supported);
                if (!sysctlbyname(feature.first, &supported, &size, nullptr, 0) && supported)
                    native_crypto |= feature.second;
            }
        }
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

    void clear_mappings(bool writes_only = false)
    {
        for (unsigned word = 0; word < populated.size(); ++word) {
            for (uint64_t pending = populated[word]; pending; pending &= pending - 1) {
                auto &entry = mappings[word * 64 + __builtin_ctzll(pending)];
                if (writes_only) entry.write_tag = UINT64_MAX;
                else entry = EmptyMapping;
            }
        }
        if (!writes_only) populated.fill(0);
    }

    static bool older_first(const CodeAge &a, const CodeAge &b) { return a.first > b.first; }

    void release_code(CompiledPage::Allocation allocation)
    {
        auto next = free_ranges.lower_bound(allocation.offset);
        if (next != free_ranges.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == allocation.offset) {
                free_sizes.erase({previous->second, previous->first});
                allocation.offset = previous->first;
                allocation.size += previous->second;
                free_ranges.erase(previous);
            }
        }
        if (next != free_ranges.end() && allocation.offset + allocation.size == next->first) {
            free_sizes.erase({next->second, next->first});
            allocation.size += next->second;
            free_ranges.erase(next);
        }
        free_ranges.emplace(allocation.offset, allocation.size);
        free_sizes.emplace(allocation.size, allocation.offset);
    }

    auto oldest_page()
    {
        while (!code_age.empty()) {
            std::pop_heap(code_age.begin(), code_age.end(), older_first);
            auto &[stamp, code] = code_age.back();
            if (stamp != code->last_used) {
                stamp = code->last_used;
                std::push_heap(code_age.begin(), code_age.end(), older_first);
                continue;
            }
            auto range = pages.equal_range(code->cache_key);
            auto found = std::find_if(range.first, range.second,
                [code](const auto &entry) { return entry.second.get() == code; });
            if (found == range.second) throw std::logic_error("Direct cache page lost");
            code_age.pop_back();
            return found;
        }
        return pages.end();
    }

    static size_t dormant_cost(const CompiledPage &code)
    {
        return sizeof(code) + code.dormant.capacity() * sizeof(uint32_t) +
            code.dormant_faults.capacity() * sizeof(FaultSite) +
            code.allocations.capacity() * sizeof(CompiledPage::Allocation) +
            code.loops.capacity() * sizeof(CompiledPage::Loop);
    }

    template<class Relocate> static void relocate_page(CompiledPage &code, Relocate relocate)
    {
        for (auto &entry : code.entries) if (entry) entry = (void *)relocate(uintptr_t(entry));
        for (auto &entry : code.branch_entries) if (entry) entry = (void *)relocate(uintptr_t(entry));
        for (auto &loop : code.loops) {
            if (loop.generic) loop.generic = (void *)relocate(uintptr_t(loop.generic));
            if (loop.fast) loop.fast = (void *)relocate(uintptr_t(loop.fast));
        }
        for (auto &site : code.dormant_faults) {
            site.instruction = relocate(site.instruction); site.slow = relocate(site.slow);
            site.patch = relocate(site.patch); site.checked = relocate(site.checked);
        }
    }

    void clear_routes(CompiledPage *code)
    {
        for (auto &route : source_routes) if (route.code == code) route = {};
        for (auto &route : routes) if (route.code == code) route = {UINT64_MAX, nullptr};
        for (auto &route : call_routes) if (route.code == code) route = {UINT64_MAX, nullptr};
    }

    void erase_page(decltype(pages)::iterator page)
    {
        auto *code = page->second.get();
        clear_routes(code);
        size_t bytes = 0;
        for (const auto &allocation : code->allocations) bytes += allocation.size;
        // Leave room for a maximum-sized extension after restoring a page.
        bool retain = dormant_limit && bytes <= capacity / 2;
        if (retain) code->dormant.reserve(bytes / 4);
        for (const auto &allocation : code->allocations) {
            auto begin = uintptr_t(executable) + allocation.offset;
            auto end = begin + allocation.size;
            auto first = std::lower_bound(fault_sites.begin(), fault_sites.end(), begin,
                [](const auto &site, uintptr_t address) { return site.instruction < address; });
            auto last = std::lower_bound(first, fault_sites.end(), end,
                [](const auto &site, uintptr_t address) { return site.instruction < address; });
            if (retain) {
                code->dormant.insert(code->dormant.end(), (const uint32_t *)begin, (const uint32_t *)end);
                code->dormant_faults.insert(code->dormant_faults.end(), first, last);
            }
            fault_sites.erase(first, last);
            release_code(allocation);
            used -= allocation.size;
        }
        if (retain) {
            relocate_page(*code, [&](uintptr_t address) {
                size_t compact = 1;
                for (const auto &allocation : code->allocations) {
                    uintptr_t begin = uintptr_t(executable) + allocation.offset;
                    if (address >= begin && address < begin + allocation.size) return compact + address - begin;
                    compact += allocation.size;
                }
                throw std::logic_error("Direct code relocation outside page");
            });
            code->allocations.clear();
            dormant_pages.push_back(code);
            code->dormant_position = std::prev(dormant_pages.end());
            dormant_bytes += dormant_cost(*code);
            while (dormant_bytes > dormant_limit) {
                auto *oldest = dormant_pages.front();
                dormant_bytes -= dormant_cost(*oldest);
                dormant_pages.pop_front();
                clear_routes(oldest);
                auto range = pages.equal_range(oldest->cache_key);
                auto found = std::find_if(range.first, range.second,
                    [oldest](const auto &entry) { return entry.second.get() == oldest; });
                pages.erase(found);
            }
            return;
        }
        pages.erase(page);
    }

    void restore_page(CompiledPage &code)
    {
        dormant_bytes -= dormant_cost(code);
        dormant_pages.erase(code.dormant_position);
        size_t bytes = code.dormant.size() * 4;
        auto range = reserve_code(bytes);
#if !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(0);
#endif
        std::memcpy((char *)writable + range.offset, code.dormant.data(), bytes);
#if !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(1);
#endif
        auto address = uintptr_t(executable) + range.offset;
        sys_icache_invalidate((void *)address, bytes);
        relocate_page(code, [address](uintptr_t offset) { return address + offset - 1; });
        auto previous = fault_sites.size();
        std::sort(code.dormant_faults.begin(), code.dormant_faults.end(),
                  [](const auto &a, const auto &b) { return a.instruction < b.instruction; });
        fault_sites.insert(fault_sites.end(), code.dormant_faults.begin(), code.dormant_faults.end());
        std::inplace_merge(fault_sites.begin(), fault_sites.begin() + previous, fault_sites.end(),
                          [](const auto &a, const auto &b) { return a.instruction < b.instruction; });
        std::vector<uint32_t>().swap(code.dormant);
        std::vector<FaultSite>().swap(code.dormant_faults);
        code.allocations.push_back({range.offset, bytes});
        free_ranges.erase(range.offset);
        free_sizes.erase({range.size, range.offset});
        if (range.size > bytes) release_code({range.offset + bytes, range.size - bytes});
        used += bytes;
        code.last_used = ++cache_clock;
        code_age.emplace_back(code.last_used, &code);
        std::push_heap(code_age.begin(), code_age.end(), older_first);
        ++restores;
    }

    CompiledPage::Allocation reserve_code(size_t required)
    {
        for (;;) {
            // Emitted size is known after branch relocation. Start with the
            // largest gap so a small fragment cannot force repeated compilation.
            if (!free_sizes.empty() && free_sizes.rbegin()->first >= required)
                return {free_sizes.rbegin()->second, free_sizes.rbegin()->first};
            if (!reclaim_cache) {
                pages.clear();
                code_age.clear();
                source_routes.fill({});
                routes.fill({UINT64_MAX, nullptr});
                call_routes.fill({UINT64_MAX, nullptr});
                fault_sites.clear();
                free_ranges.clear();
                free_sizes.clear();
                release_code({0, capacity});
                used = 0;
                ++flushes;
            } else {
                auto oldest = oldest_page();
                if (oldest == pages.end()) throw std::logic_error("Direct cache allocation lost");
                // A page owns all of its emitted blocks, including direct local
                // branches between blocks. Recycle them together after leaving JIT.
                erase_page(oldest);
                ++evictions;
            }
        }
    }

    CompiledPage &code_page(uint64_t page, const void *bytes, uint32_t flags, size_t entry,
                            uint64_t version)
    {
        auto same_content = [&](const CompiledPage &code) {
            ++code_checks;
            return !std::memcmp(code.source.data(), bytes, 4096);
        };
        unsigned slot = ((uintptr_t(bytes) >> 12) ^ flags) & 255;
        auto &route = source_routes[slot];
        bool known = route.source == bytes && route.flags == flags && route.code &&
            (share_code || route.code->first_address == page) &&
            ((version && route.version == version) || same_content(*route.code));
        if (known && version && route.version == version) ++code_version_hits;
        if (known) route.version = version;
        if (known && route.code->entries[entry]) {
            route.code->last_used = ++cache_clock;
            shared_hits += route.code->first_address != page;
            return *route.code;
        }
        uint64_t key = known ? route.code->cache_key : UINT64_C(0xcbf29ce484222325);
        if (!known) {
            for (size_t i = 0; i < 4096; i += 8) {
                uint64_t word;
                std::memcpy(&word, static_cast<const char *>(bytes) + i, 8);
                key = (key ^ word) * UINT64_C(0x100000001b3);
            }
            key ^= uint64_t(flags) * UINT64_C(0x9e3779b97f4a7c15);
            if (!share_code) key ^= page * UINT64_C(0x9e3779b97f4a7c15);
        }
        auto find = [&] {
            auto range = pages.equal_range(key);
            for (auto found = range.first; found != range.second; ++found) {
                const auto &compiled = *found->second;
                // Hash equality only narrows the lookup. Exact bytes and mode
                // are required; permissions still come from the current owner.
                if (compiled.compile_flags == flags &&
                    (share_code || compiled.first_address == page) &&
                    ((known && route.code == &compiled) || same_content(compiled))) return found;
            }
            return pages.end();
        };
        auto found = find();
        if (found != pages.end() && !found->second->dormant.empty()) restore_page(*found->second);
        if (found != pages.end() && found->second->entries[entry]) {
            found->second->last_used = ++cache_clock;
            shared_hits += found->second->first_address != page;
            route = {bytes, found->second.get(), flags, version};
            return *found->second;
        }
        size_t required = 16;
        for (;;) {
            auto range = reserve_code(required);
            found = find();
            if (found != pages.end() && !found->second->dormant.empty()) {
                restore_page(*found->second);
                continue;
            }
            if (found == pages.end()) {
                auto compiled = std::make_unique<CompiledPage>();
                compiled->compile_flags = flags;
                compiled->first_address = page;
                compiled->cache_key = key;
                compiled->last_used = ++cache_clock;
                std::memcpy(compiled->source.data(), bytes, 4096);
                found = pages.emplace(key, std::move(compiled));
                code_age.emplace_back(found->second->last_used, found->second.get());
                std::push_heap(code_age.begin(), code_age.end(), older_first);
            }
            auto &compiled = found->second;
            CompiledPage candidate(*compiled);
            size_t emitted = candidate.compile(0, bytes, entry, executable + range.offset / 4,
                                              writable + range.offset / 4, flags, fault_sites, range.size,
                                              compiled->branch_entries.data());
            emitted = (emitted + 15) & ~size_t(15);
            if (emitted > range.size) { required = emitted; continue; }
            candidate.allocations.push_back({range.offset, emitted});
            candidate.last_used = ++cache_clock;
            *compiled = std::move(candidate);
            free_ranges.erase(range.offset);
            free_sizes.erase({range.size, range.offset});
            if (range.size > emitted) release_code({range.offset + emitted, range.size - emitted});
            used += emitted;
            ++compiles;
            route = {bytes, compiled.get(), flags, version};
            return *compiled;
        }
    }
};

HuskArm64Direct *husk_direct_create(size_t cache_bytes)
{
    if (!cache_bytes) {
        unsigned long mib = 32;
        if (const char *setting = std::getenv("HUSK_ARM64_CODE_CACHE_MIB")) {
            char *end;
            errno = 0;
            mib = std::strtoul(setting, &end, 10);
            if (errno || end == setting || *end || mib < 2 || mib > 256)
                throw std::invalid_argument("HUSK_ARM64_CODE_CACHE_MIB must be 2..256");
        }
        cache_bytes = mib * 1024 * 1024;
    }
    return new HuskArm64Direct(cache_bytes);
}
HuskArm64CacheStats husk_direct_cache_stats(const HuskArm64Direct *ctx)
{
    return {ctx->compiles, ctx->evictions, ctx->flushes, ctx->shared_hits, ctx->used, ctx->pages.size(),
            ctx->code_version_hits, ctx->code_checks, ctx->restores, ctx->dormant_bytes};
}
bool husk_direct_supported(uint32_t instruction) { return decode(instruction) != Other; }
bool husk_arm64_landing_supported(uint32_t instruction)
{
    Kind kind = decode(instruction);
    return instruction == 0xd503233f || instruction == 0xd503237f ||
        (kind != Other && kind != Assist && kind != Pauth && !(kind >= Branch && kind <= Indirect));
}
void husk_direct_destroy(HuskArm64Direct *ctx) { delete ctx; }

HuskArm64Exit husk_direct_run(HuskArm64Direct *ctx, HuskArm64State *state,
                            uint64_t page, const void *bytes,
                            HuskArm64Memory memory, uint64_t budget)
{
    auto started = ctx->profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if ((state->fpcr & ~HUSK_FPCR_MASK) || state->pc < page || state->pc - page >= 4096 || (state->pc & 3))
        return HUSK_ARM64_FALLBACK;
    if ((state->access_flags & HUSK_BTYPE_MASK) &&
        !husk_arm64_landing_supported(static_cast<const uint32_t *>(bytes)[(state->pc - page) / 4]))
        return HUSK_ARM64_FALLBACK;
    uint32_t access_flags = (state->access_flags & (ctx->native_lse ? 15 : 11)) | (ctx->fastmem ? CompileFastmem : 0);
    access_flags |= state->access_flags & HUSK_MOPS_ALLOWED;
    if (ctx->inline_assists) access_flags |= CompileInlineAssists;
    if (ctx->inline_assists && ctx->tail_cache) access_flags |= CompileTail;
    if (ctx->native_lse && ctx->exclusive) access_flags |= CompileExclusive;
    if (ctx->native_crc) access_flags |= state->access_flags & 64;
    if (ctx->verify_pauth && memory.assist) access_flags |= CompilePauthVerify;
    if (memory.kernel_reads) access_flags |= CompileKernelReads;
    if (ctx->shared_stubs) access_flags |= CompileSharedStubs;
    if (state->access_flags & HUSK_AES) access_flags |= ctx->native_crypto & CompileAes;
    if (state->access_flags & HUSK_SHA1) access_flags |= ctx->native_crypto & CompileSha1;
    if (state->access_flags & HUSK_SHA256) access_flags |= ctx->native_crypto & CompileSha256;
    if (state->access_flags & HUSK_PAUTH_HINTS_NOP) access_flags |= CompilePauthNop;
    if (ctx->dormant_limit) access_flags |= CompileRelocatable;
    bool same_mappings = memory.mapping_epoch && memory.mapping_epoch == ctx->mapping_epoch &&
        memory.opaque == ctx->mapping_owner && memory.map_page == ctx->mapping_callback;
    ctx->mapping_epoch = memory.mapping_epoch;
    ctx->mapping_owner = memory.opaque;
    ctx->mapping_callback = memory.map_page;
    if (ctx->fastmem) {
        if (!same_mappings) ctx->fastmem->reset();
        ctx->fastmem->protect_code(bytes);
    }
    if (ctx->version_owner != memory.opaque || ctx->version_callback != memory.code_version) {
        ctx->source_routes.fill({});
        ctx->version_owner = memory.opaque;
        ctx->version_callback = memory.code_version;
    }
    auto resolve = [&](uint64_t address, const void *contents, bool landing) -> CompiledPage * {
        auto compile_start = ctx->profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto previous_compiles = ctx->compiles;
        auto *compiled = &ctx->code_page(address & ~UINT64_C(4095), contents,
                                         access_flags | (landing ? CompileLanding : 0), (address & 4095) / 4,
                                         memory.code_version ? memory.code_version(memory.opaque, contents) : 0);
        if (ctx->profile && previous_compiles != ctx->compiles)
            ctx->compile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - compile_start).count();
        if (!landing)
            ctx->routes[(address >> 12) & 255] = {address >> 12, compiled->branch_entries.data(), compiled, 0};
        return compiled;
    };
    ctx->routes.fill({UINT64_MAX, nullptr});
    ctx->call_routes.fill({UINT64_MAX, nullptr});
    auto *code = resolve(state->pc, bytes, state->access_flags & HUSK_BTYPE_MASK);
    // The new entry page can alias an earlier writable mapping. Read mappings
    // survive only while the owner keeps its mapping generation unchanged.
    ctx->clear_mappings(same_mappings);
    ctx->pauth_cache.fill({});
    Frame frame{*state, std::min(budget, uint64_t(INT64_MAX)), 0, 0, 0,
                ctx->routes.data(), ctx->call_routes.data(),
                ctx->fastmem ? ctx->fastmem->bases(false) : nullptr,
                ctx->fastmem ? ctx->fastmem->bases(true) : nullptr, ctx->pauth_cache.data(), nullptr, 0, 0,
                page, code, memory.pauth, memory.kernel_reads,
                husk_direct_exit_stub, husk_direct_branch_stub};
    void *next = code->entries[(state->pc - page) / 4];
    for (;;) {
#if !TARGET_OS_IPHONE
        // QEMU translation, linking or an MMU callback can leave this thread
        // in MAP_JIT write mode, including when the Direct code is cached.
        pthread_jit_write_protect_np(1);
#endif
        if (ctx->fastmem) {
            frame.fault_sites = ctx->fault_sites.data();
            frame.fault_count = ctx->fault_sites.size();
            Frame *expected = nullptr;
            if (!active_fastmem.compare_exchange_strong(expected, &frame))
                throw std::runtime_error("fastmem requires serialized execution");
        }
        frame.cache_clock = ctx->cache_clock;
        husk_direct_enter(&frame, next, ctx->mappings.data());
        if (ctx->fastmem) active_fastmem.store(nullptr);
        // Native route hits bypass code_page(). The page that just exited is
        // active even when no host lookup has refreshed its LRU timestamp.
        frame.code->last_used = ++ctx->cache_clock;
        if (frame.reason != CodeBranch) {
            page = frame.code_page;
            code = frame.code;
        }
        HuskArm64Assist assisted = HUSK_ASSIST_DECLINED;
        uint32_t assisted_insn = 0;
        uint64_t call_target = UINT64_MAX, call_source = 0;
        bool cached_jump = false;
        if ((frame.reason == Fallback || frame.reason == PauthCheck) && memory.assist) {
            uint32_t word = code->source[(frame.state.pc - page) / 4];
            assisted_insn = word;
            bool pauth = decode(word) == Pauth;
            unsigned pauth_reg = pauth_register(word);
            uint64_t pauth_value = pauth ? frame.state.x[pauth_reg] : 0;
            uint64_t pauth_modifier = !pauth ? 0 : pauth_reg == 17 ? frame.state.x[16] :
                word != 0xd50320ff && (word & 32) ? frame.state.sp : 0;
            uint64_t bulk_bytes = word == HUSK_BULK_INSN ? frame.state.x[2] :
                husk_arm64_mops_prologue(word) && ((word >> 5) & 31) < 31 ?
                    frame.state.x[(word >> 5) & 31] : 0;
            if ((word & 0xfffffc1f) == 0xd63f0000 && ((word >> 5) & 31) != 31)
                call_target = frame.state.x[(word >> 5) & 31];
            if (ctx->inline_assists && ctx->tail_cache &&
                (word & 0xfffffc1f) == 0xd61f0000 && ((word >> 5) & 31) != 31) {
                unsigned reg = (word >> 5) & 31;
                call_target = frame.state.x[reg];
                call_source = (reg == 16 || reg == 17) ? 1 : frame.code_page | 3;
                cached_jump = true;
            }
            assisted = memory.assist(memory.opaque, &frame.state, word);
            if (frame.reason == PauthCheck &&
                (assisted != HUSK_ASSIST_PAUTH || frame.state.x[pauth_reg] != frame.address))
                throw std::runtime_error("native PAUTH differs from owner result");
            if (frame.reason == PauthCheck) {
                ++ctx->pauth_checks;
                if (!(ctx->pauth_checks & (ctx->pauth_checks - 1)))
                    std::fprintf(stderr, "[husk-native-pauth] verified=%llu\n",
                                 (unsigned long long)ctx->pauth_checks);
            }
            // A PAC landing keeps BTYPE until authentication/signing succeeds.
            // The owner exports the pending state before any guest exception.
            if (pauth && assisted == HUSK_ASSIST_PAUTH && (code->compile_flags & CompileLanding))
                frame.state.access_flags &= ~uint32_t(HUSK_BTYPE_MASK);
            if (ctx->inline_assists && pauth && assisted == HUSK_ASSIST_PAUTH)
                ctx->pauth_cache[pauth_slot(word, pauth_value, pauth_modifier)] =
                    {pauth_value, pauth_modifier, frame.state.x[pauth_reg], word};
            if ((word == HUSK_BULK_INSN || husk_arm64_mops_prologue(word)) && assisted != HUSK_ASSIST_DECLINED) {
                // Bulk calls consume a byte-weighted budget so repeated host
                // copies return to the owner for pending interrupts.
                frame.budget -= (word == HUSK_BULK_INSN ? 1 : 3) +
                    std::min(bulk_bytes, uint64_t(HUSK_BULK_LIMIT)) / 16;
                if (int64_t(frame.budget) < 0) { frame.reason = Budget; break; }
            }
            // A skipped BTI at the end of a page needs two immutable pages.
            // Such targets, and tagged addresses, keep the checked slow path.
            if ((cached_jump ? assisted != HUSK_ASSIST_CHAIN_JUMP && assisted != HUSK_ASSIST_CHAIN_LANDING_JUMP :
                               assisted != HUSK_ASSIST_CHAIN_CALL && assisted != HUSK_ASSIST_CHAIN_LANDING_CALL) ||
                (call_target & ~UINT64_C(4095)) != (frame.state.pc & ~UINT64_C(4095)))
                call_target = UINT64_MAX;
        }
        if (assisted != HUSK_ASSIST_DECLINED) {
            ++ctx->assists;
            if (ctx->profile && !(ctx->assists & 4095)) ++ctx->assist_samples[assisted_insn];
            if (assisted == HUSK_ASSIST_BRANCH) { frame.reason = CodeBranch; break; }
            if (assisted == HUSK_ASSIST_CHAIN || assisted == HUSK_ASSIST_CHAIN_CALL ||
                assisted == HUSK_ASSIST_CHAIN_LANDING_CALL || assisted == HUSK_ASSIST_CHAIN_JUMP ||
                assisted == HUSK_ASSIST_CHAIN_LANDING_JUMP) {
                ++ctx->chained_assists;
                // An indirect target can enter in the middle of a static block.
                // Charge the branch even when that target has no block budget.
                --frame.budget;
                if (int64_t(frame.budget) < 0) { frame.reason = Budget; break; }
                frame.reason = CodeBranch;
            } else if (frame.state.pc < page || frame.state.pc - page >= 4096 ||
                !code->entries[(frame.state.pc - page) / 4]) {
                frame.reason = CodeBranch;
            } else {
                next = code->entries[(frame.state.pc - page) / 4];
                continue;
            }
        }
        if (frame.reason == CodeBranch) {
            if (!memory.map_code || (frame.state.pc & 3) || int64_t(frame.budget) < 0) break;
            uint64_t destination = frame.state.pc & ~UINT64_C(4095);
            const auto &route = ctx->routes[(destination >> 12) & 255];
            if (!(frame.state.access_flags & HUSK_BTYPE_MASK) && route.tag == destination >> 12 &&
                route.entries[(frame.state.pc & 4095) / 4]) {
                // Only immutable code pages from this run are present here.
                next = route.entries[(frame.state.pc & 4095) / 4];
                frame.code_page = destination;
                frame.code = route.code;
                if (call_target != UINT64_MAX)
                    ctx->call_routes[call_slot(call_target, cached_jump)] =
                        {call_target, next, destination, route.code, call_source, 0, {}};
                continue;
            }
            const void *contents = memory.map_code(memory.opaque, destination);
            if (!contents) break;
            // A previously writable data page can now be executable. Recheck
            // writes so every code page visited in this slice remains immutable.
            ctx->clear_mappings(true);
            if (ctx->fastmem) ctx->fastmem->protect_code(contents);
            code = resolve(frame.state.pc, contents, frame.state.access_flags & HUSK_BTYPE_MASK);
            page = destination;
            frame.code_page = page;
            frame.code = code;
            next = code->branch_entries[(frame.state.pc - page) / 4];
            if (call_target != UINT64_MAX)
                ctx->call_routes[call_slot(call_target, cached_jump)] = {call_target, next,
                    page, code, call_source, frame.state.access_flags & HUSK_BTYPE_MASK, {}};
            continue;
        }
        if (frame.reason == Budget || frame.reason == Fallback) break;
        if (frame.reason == FastReadPage || frame.reason == FastWritePage) {
            bool write = frame.reason == FastWritePage;
            if (!ctx->fastmem->map(frame.address, write, memory, access_flags & 8)) {
                // One incompatible access switches this instruction to its checked
                // implementation. Future accesses do not repeat a host fault.
                const auto &site = ctx->fault_sites.at(frame.fault_index);
                int64_t distance = int64_t(site.checked) - int64_t(site.patch);
                if (distance % 4 || distance < -(INT64_C(1) << 27) || distance >= (INT64_C(1) << 27))
                    throw std::runtime_error("fastmem fallback branch out of range");
                auto *word = ctx->writable + (site.patch - uintptr_t(ctx->executable)) / 4;
#if !TARGET_OS_IPHONE
                pthread_jit_write_protect_np(0);
#endif
                *word = 0x14000000 | (uint32_t(distance / 4) & 0x3ffffff);
#if !TARGET_OS_IPHONE
                pthread_jit_write_protect_np(1);
#endif
                sys_icache_invalidate(reinterpret_cast<void *>(site.patch), 4);
            }
            next = code->entries[(frame.state.pc - page) / 4];
            continue;
        }
        if (frame.reason == PrepareLoop) {
            const auto &loop = *std::find_if(code->loops.begin(), code->loops.end(), [&](const auto &l) {
                return page + l.begin * 4 == frame.state.pc;
            });
            next = loop.generic;
            uint64_t base = frame.state.x[loop.base];
            if (memory.map_page && (code->unaligned || !(base & (loop.alignment - 1))) &&
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
        unsigned slot = (tag ^ (tag >> 8)) & 255;
        auto &entry = ctx->mappings[slot];
        ctx->populated[slot / 64] |= UINT64_C(1) << (slot % 64);
        (write ? entry.write_tag : entry.read_tag) = tag;
        (write ? entry.write_addend : entry.read_addend) = uintptr_t(host) - base;
        next = code->entries[(frame.state.pc - page) / 4];
    }
    *state = frame.state;
    if (ctx->profile) {
        auto now = std::chrono::steady_clock::now();
        ++ctx->calls;
        ctx->branches += frame.reason == CodeBranch;
        ctx->budget_exits += frame.reason == Budget;
        if (frame.reason == Fallback && state->pc >= page && state->pc - page < 4096)
            ++ctx->fallbacks[code->source[(state->pc - page) / 4]];
        ctx->run_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(now - started).count();
        if (now - ctx->report > std::chrono::seconds(5)) {
            std::fprintf(stderr, "[husk-direct-stats] calls=%llu compiles=%llu compile_ms=%.3f run_ms=%.3f pages=%zu code_kb=%zu flushes=%llu evictions=%llu shared_hits=%llu branches=%llu budgets=%llu assists=%llu chained_assists=%llu code_version_hits=%llu code_checks=%llu restores=%llu cold_kb=%zu pc=%llx\n",
                (unsigned long long)ctx->calls, (unsigned long long)ctx->compiles,
                ctx->compile_ns / 1e6, ctx->run_ns / 1e6, ctx->pages.size(), ctx->used / 1024,
                (unsigned long long)ctx->flushes, (unsigned long long)ctx->evictions,
                (unsigned long long)ctx->shared_hits, (unsigned long long)ctx->branches,
                (unsigned long long)ctx->budget_exits, (unsigned long long)ctx->assists,
                (unsigned long long)ctx->chained_assists,
                (unsigned long long)ctx->code_version_hits, (unsigned long long)ctx->code_checks,
                (unsigned long long)ctx->restores, ctx->dormant_bytes / 1024,
                (unsigned long long)state->pc);
            auto histogram = [](const char *label, const auto &counts) {
                std::vector<std::pair<uint64_t, uint32_t>> hottest;
                for (auto [word, count] : counts) hottest.emplace_back(count, word);
                std::partial_sort(hottest.begin(), hottest.begin() + std::min(size_t(8), hottest.size()), hottest.end(), std::greater<>());
                std::fprintf(stderr, "[%s]", label);
                for (size_t i = 0; i < std::min(size_t(8), hottest.size()); ++i)
                    std::fprintf(stderr, " %08x:%llu", hottest[i].second, (unsigned long long)hottest[i].first);
                std::fprintf(stderr, "\n");
            };
            histogram("husk-direct-fallback", ctx->fallbacks);
            histogram("husk-direct-assist-sample-4096", ctx->assist_samples);
            ctx->report = now;
        }
    }
    return frame.reason == Budget ? HUSK_ARM64_BUDGET :
           frame.reason == CodeBranch ? HUSK_ARM64_BRANCH : HUSK_ARM64_FALLBACK;
}
