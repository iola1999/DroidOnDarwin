/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "internal.h"
#include "decode.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <oaknut/oaknut.hpp>
namespace nce_direct
{
using namespace oaknut;
using namespace oaknut::util;
static bool resident(unsigned reg)
{
    return reg < 16 || (reg >= 19 && reg < 26);
}
size_t Page::compile(uint64_t new_page, const void *bytes, size_t entry,
                     uint32_t *executable, uint32_t flags, void **stable_entries, unsigned mapping_mask)
{
    bool unaligned = flags & 1;
    unsigned page_bits = flags & CompileData16K ? 14 : 12;
    std::array<uint32_t, 1024> input;
    std::memcpy(input.data(), bytes, 4096);
    std::array<bool, 1024> emit{};
    std::vector<size_t> pending{entry};
    while (!pending.empty()) {
        size_t i = pending.back();
        pending.pop_back();
        for (; i < input.size() && !emit[i] && !entries[i]; ++i) {
            emit[i] = true;
            Kind k = decode(input[i]);
            if (k >= Branch && k <= TestBranch) {
                uint64_t address = target(input[i], k, new_page + i * 4);
                if (address >= new_page && address - new_page < 4096)
                    pending.push_back((address - new_page) / 4);
            }
            if (k == Other || k == Indirect ||
                (k == Branch && !(input[i] & 0x80000000)))
                break;
        }
    }
    size_t first = 0, limit = input.size();
    while (!emit[first])
        ++first;
    while (!emit[limit - 1])
        --limit;
    first &= ~size_t(31);
    limit = (limit + 31) & ~size_t(31);
    std::vector<Label> labels(limit - first + 1), exhausted(limit - first);
    std::array<bool, 1025> leaders{};
    leaders[1024] = true;
    for (size_t i = first; i < limit; ++i) {
        if (!emit[i])
            continue;
        if (!(i % 32) || i == entry || (i && !emit[i - 1]))
            leaders[i] = true;
        Kind k = decode(input[i]);
        if (k >= Branch && k <= TestBranch) {
            uint64_t address = target(input[i], k, new_page + i * 4);
            if (address >= new_page && address - new_page < 4096)
                leaders[(address - new_page) / 4] = true;
        }
        if (k == Other || (k >= Branch && k <= Indirect))
            leaders[i + 1] = true;
    }
    std::vector<uint32_t> output;
    VectorCodeGenerator c(output, executable);
    auto relative_address = [&](XReg reg, uint64_t offset, XReg scratch) {
        c.LDR(reg, X27, offsetof(Frame, code_page));
        if (!offset)
            return;
        if (offset <= 4095)
            c.ADD(reg, reg, offset);
        else if (offset >= UINT64_MAX - 4094)
            c.SUB(reg, reg, -offset);
        else {
            c.MOV(scratch, offset);
            c.ADD(reg, reg, scratch);
        }
    };
    auto exit = [&](uint64_t pc, Stop reason) {
        c.MOV(W17, uint32_t(((pc - new_page) << 4) | reason));
        c.LDR(X26, X27, DIRECT_EXIT_STUB);
        c.BR(X26);
    };
    auto load = [&](unsigned r, XReg to, bool sp = false) {
        if (resident(r))
            c.MOV(to, XReg(r));
        else if (r == 31 && !sp)
            c.MOV(to, 0);
        else
            c.LDR(to, X27, r * 8);
    };
    auto save = [&](unsigned r, XReg from, bool sp = false) {
        if (resident(r))
            c.MOV(XReg(r), from);
        else if (r != 31 || sp)
            c.STR(from, X27, r * 8);
    };
    // X16 holds a guest target. The owner and guest page travel with a
    // cached destination, allowing identical code at different guest VAs.
    auto dynamic_jump = [&] {
        c.LDR(X26, X27, DIRECT_BRANCH_STUB);
        c.BR(X26);
    };
    auto jump = [&](uint64_t pc) {
        if (pc >= new_page && pc - new_page < 4096) {
            size_t index = (pc - new_page) / 4;
            if (emit[index]) {
                c.B(labels[index - first]);
                return;
            }
            if (branch_entries[index]) {
                c.MOV(X16, uint64_t(stable_entries));
                c.LDR(X16, X16, index * sizeof(void *));
                c.BR(X16);
                return;
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
            if (resident(r) || (r == 31 && !sp))
                return r;
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
        if (kind != MoveWide)
            n = operand(rn, kind == AddImm || kind == AddExt);
        if (two_sources)
            m = operand(rm);
        if (kind == Multiply)
            a = operand(ra);
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
        if (kind != MoveWide)
            w = (w & ~(31U << 5)) | (n << 5);
        if (two_sources)
            w = (w & ~(31U << 16)) | (m << 16);
        if (kind == Multiply)
            w = (w & ~(31U << 10)) | (a << 10);
        c.dw(w);
        if (!resident(rd) && (rd != 31 || dest_sp))
            c.STR(XReg(d), X27, rd * 8);
    };
    auto translate = [&](bool write, Label &miss) {
        if (flags & CompileFlat) {
            // Keep NZCV intact and validate upper address bits before indexing.
            c.LDR(X26, X27, DIRECT_PTE_USER_BASE);
            c.SUB(X17, X16, X26);
            c.LSR(X26, X17, DIRECT_FLAT_USER_BITS);
            c.CBNZ(X26, miss);
            c.LSR(X17, X17, 14);
            c.ADD(X17, X28, X17, LSL, 4);
            c.LDR(X17, X17, write ? 8 : 0);
            c.CBZ(X17, miss);
            if (write) {
                c.LDR(X26, X27, DIRECT_PTE_RAM);
                c.SUB(X26, X17, X26);
                c.LSR(X26, X26, 14);
                c.LDR(X30, X27, DIRECT_PTE_CODE_RUNS);
                c.LDR(X26, X30, X26, LSL, 3);
                c.LDR(X30, X27, DIRECT_RUN_ID);
                c.EOR(X26, X26, X30);
                c.CBZ(X26, miss);
            }
            c.AND(X26, X16, 16383);
            c.ADD(X17, X17, X26);
            return;
        }
        if ((flags & CompilePageTable) && !(flags & CompilePteReadRefill)) {
            c.LDR(X26, X27, write ? DIRECT_PTE_WRITE_STUB : DIRECT_PTE_READ_STUB);
            c.BLR(X26);
            c.CBZ(X17, miss);
            return;
        }
        c.LSR(X17, X16, page_bits);
        c.EOR(X17, X17, X17, LSR, 8);
        c.AND(X17, X17, mapping_mask);
        c.ADD(X17, X28, X17, LSL, 5);
        c.LDP(X26, X17, X17, write ? 16 : 0);
        c.EOR(X26, X26, X16, LSR, page_bits);
        if (!write && (flags & CompilePteReadRefill)) {
            Label cached, done;
            c.CBZ(X26, cached);
            c.LDR(X26, X27, DIRECT_PTE_READ_STUB);
            c.BLR(X26);
            c.CBZ(X17, miss);
            c.B(done);
            c.l(cached);
            c.ADD(X17, X16, X17);
            c.l(done);
        } else {
            c.CBNZ(X26, miss);
            c.ADD(X17, X16, X17);
        }
    };
    for (size_t i = first; i < limit; ++i) {
        if (emit[i]) {
            uint64_t pc = new_page + i * 4;
            c.l(labels[i - first]);
            branch_entries[i] = c.xptr<void *>();
            if (leaders[i]) {
                c.LDR(X16, X27, DIRECT_STOP_REQUEST);
                c.LDAR(W16, X16);
                c.CBNZ(W16, exhausted[i - first]);
                size_t end = i + 1;
                while (!leaders[end] && emit[end])
                    ++end;
                c.SUB(X29, X29, end - i);
                c.TBNZ(X29, 63, exhausted[i - first]);
            }
            // Slow-path re-entry already charged this block's execution budget.
            entries[i] = c.xptr<void *>();
            uint32_t w = input[i];
            unsigned rd = w & 31, rn = (w >> 5) & 31;
            Kind kind = decode(w);
            if (kind == Atomic && !(flags & 4))
                kind = Other;
            if (kind == Crc && !(flags & 64))
                kind = Other;
            if ((kind == CryptoAes && !(flags & CompileAes)) ||
                (kind == CryptoSha1 && !(flags & CompileSha1)) ||
                (kind == CryptoSha256 && !(flags & CompileSha256)))
                kind = Other;
            if ((kind == ExclusiveLoad || kind == ExclusiveStore || kind == ClearExclusive) &&
                !(flags & CompileExclusive))
                kind = Other;
            if (integer_kind(kind)) {
                integer(w, kind, 26);
            } else if (kind == Pauth && (flags & CompilePauthNop)) {
                // Only the architectural HINT encodings become NOPs.
            } else if (kind == Barrier) {
                c.dw(0xd5033bbf);  // dmb ish; accesses are normal RAM.
            } else if (kind == Bti) {
                // This executor exposes no guarded-page feature to Linux.
            } else if (kind == ClearExclusive) {
                c.MOV(X16, UINT64_MAX);
                c.STR(X16, X27, DIRECT_EXCLUSIVE_ADDR);
            } else if (kind == Hint) {
                // The guest NOP has no architectural side effects.
            } else if (kind == Dczid) {
                if (flags & CompileZva64) {
                    c.MOV(X16, 4);
                    save(rd, X16);
                } else
                    exit(pc, Fallback);
            } else if (kind == CacheZero && (flags & CompileZva64)) {
                Label miss, done;
                load(rd, X16);
                c.AND(X16, X16, ~UINT64_C(63));
                translate(true, miss);
                for (unsigned offset = 0; offset < 64; offset += 16)
                    c.STP(XZR, XZR, X17, offset);
                c.B(done);
                c.l(miss);
                c.STR(X16, X27, DIRECT_ADDRESS);
                exit(pc, WritePage);
                c.l(done);
            } else if (kind == SystemRead) {
                if ((w & 0xffffffe0) == 0xd53bd040)
                    c.LDR(X16, X27, DIRECT_TPIDR);
                else
                    c.dw((w & ~31U) | 16);
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
                relative_address(X16, value - new_page, X17);
                save(rd, X16);
            } else if (kind >= Branch && kind <= TestBranch) {
                uint64_t dest = target(w, kind, pc);
                if (kind == Branch) {
                    if (w >> 31) {
                        relative_address(X16, pc + 4 - new_page, X17);
                        save(30, X16);
                    }
                    jump(dest);
                } else {
                    Label outside, next;
                    bool in_page = dest >= new_page && dest - new_page < 4096;
                    bool near_branch = in_page && emit[(dest - new_page) / 4] && kind != TestBranch;
                    Label &taken = near_branch ? labels[(dest - new_page) / 4 - first] : outside;
                    if (kind == Conditional)
                        c.B(Cond(w & 15), taken);
                    else {
                        load(rd, X16);
                        if (kind == TestBranch) {
                            unsigned bit = ((w >> 26) & 32) | ((w >> 19) & 31);
                            if (w & (1U << 24))
                                c.TBNZ(X16, bit, taken);
                            else
                                c.TBZ(X16, bit, taken);
                        } else if (w >> 31) {
                            if (w & (1U << 24))
                                c.CBNZ(X16, taken);
                            else
                                c.CBZ(X16, taken);
                        } else {
                            if (w & (1U << 24))
                                c.CBNZ(W16, taken);
                            else
                                c.CBZ(W16, taken);
                        }
                    }
                    if (!near_branch) {
                        c.B(next);
                        c.l(outside);
                        jump(dest);
                        c.l(next);
                    }
                }
            } else if (kind == Indirect) {
                Label budget_end;
                c.LDR(X16, X27, DIRECT_STOP_REQUEST);
                c.LDAR(W16, X16);
                c.CBNZ(W16, budget_end);
                // A dynamic target can be inside a static block. Charge each
                // return too, so a self-return cannot bypass the slice budget.
                c.SUB(X29, X29, 1);
                c.TBNZ(X29, 63, budget_end);
                load(rn, X16);
                if ((w & 0xfffffc1f) == 0xd63f0000) {
                    relative_address(X17, pc + 4 - new_page, X26);
                    save(30, X17);
                }
                dynamic_jump();
                c.l(budget_end);
                exit(pc, Budget);
            } else if (kind == ExclusiveLoad || kind == ExclusiveStore) {
                bool write = kind == ExclusiveStore;
                unsigned size = w >> 30, status = (w >> 16) & 31;
                Label failed, miss, invalid, done, completed;
                load(rn, X16, true);
                if (write) {
                    if (w & 0x8000)
                        c.dw(0xd5033bbf);  // release also orders a failed STLXR
                    c.MOV(X26, X16);
                    c.LDR(X17, X27, DIRECT_EXCLUSIVE_ADDR);
                    c.EOR(X17, X17, X26);
                    c.CBNZ(X17, failed);
                }
                // Failed reservations do not issue a memory access.
                if (rn == 31) {
                    c.AND(X17, X16, 15);
                    c.CBNZ(X17, invalid);
                }
                if (size) {
                    c.AND(X17, X16, (1U << size) - 1);
                    c.CBNZ(X17, invalid);
                }
                // A software reservation survives mapping callbacks and slice exits.
                translate(write, miss);
                if (write) {
                    c.LDR(X26, X27, DIRECT_EXCLUSIVE_VALUE);
                    load(rd, X30);
                    c.dw((size << 30) | 0x08e0fc00 | (26 << 16) | (17 << 5) | 30);  // casal
                    c.LDR(X17, X27, DIRECT_EXCLUSIVE_VALUE);
                    c.EOR(X17, X17, X26);
                    c.CBNZ(X17, failed);
                    c.MOV(X26, 0);
                    c.B(completed);
                    c.l(failed);
                    c.MOV(X26, 1);
                    c.l(completed);
                    save(status, X26);
                    c.MOV(X26, UINT64_MAX);
                    c.STR(X26, X27, DIRECT_EXCLUSIVE_ADDR);
                } else {
                    c.dw((size << 30) | 0x08dffc00 | (17 << 5) | 26);  // ldar, stronger than relaxed LDXR
                    c.STR(X26, X27, DIRECT_EXCLUSIVE_VALUE);
                    c.MOV(X17, X16);
                    c.STR(X17, X27, DIRECT_EXCLUSIVE_ADDR);
                    save(rd, X26);
                }
                c.B(done);
                c.l(miss);
                c.STR(X16, X27, DIRECT_ADDRESS);
                exit(pc, write ? WritePage : ReadPage);
                c.l(invalid);
                exit(pc, Alignment);
                c.l(done);
            } else if (kind == Memory || kind == Pair || kind == Atomic || kind == Structure ||
                       kind == StructureLane || kind == Literal) {
                bool pair = kind == Pair;
                bool atomic = kind == Atomic;
                bool structure_lane = kind == StructureLane;
                bool structure = kind == Structure || structure_lane;
                bool literal = kind == Literal;
                bool ordered = atomic || (!pair && !structure && !literal && (w & 0x3fbffc00) == 0x089ffc00);
                bool vector = w & 0x04000000;
                bool write = !literal && (atomic || (pair || vector || ordered ? !(w & (1U << 22)) : ((w >> 22) & 3) == 0));
                bool reg_offset = !structure && !pair && !literal && (w & 0x3b200c00) == 0x38200800;
                bool indexed = !structure && !pair && !literal && !ordered && !reg_offset && (w & 0x3b000000) != 0x39000000;
                unsigned mode = structure ? ((w >> 23) & 1) : pair  ? (((w >> 23) & 1) ? ((w >> 23) & 3) : 0)
                                                          : indexed ? (w >> 10) & 3
                                                                    : 0;
                unsigned size_log2 = structure ? (w >> 10) & 3 : pair ? 2 + (vector ? w >> 30 : w >> 31)
                                                                      : (w >> 30) + (vector ? ((w >> 23) & 1) * 4 : 0);
                if (literal)
                    size_log2 = 2 + (vector ? w >> 30 : (w >> 30) & 1);
                if (structure_lane) {
                    unsigned scale = (w >> 14) & 3;
                    size_log2 = scale == 3 ? size_log2 : scale == 2 && (size_log2 & 1) ? 3
                                                                                       : scale;
                }
                unsigned access_bytes = structure_lane ? structure_lane_bytes(w) : structure ? structure_registers(w) * (w & (1U << 30) ? 16 : 8)
                                                                                             : (pair ? 2U : 1U) << size_log2;
                int64_t offset = structure || literal ? 0 : pair                ? sign_extend((w >> 15) & 127, 7) * (1 << size_log2)
                                                        : reg_offset || ordered ? 0
                                                        : indexed               ? sign_extend((w >> 12) & 511, 9)
                                                                                : ((w >> 10) & 4095) << size_log2;
                Label miss, invalid, crossing, done;
                if (literal)
                    relative_address(X16, pc - new_page + sign_extend((w >> 5) & 0x7ffff, 19) * 4, X17);
                else
                    load(rn, X16, true);
                if (!literal && rn == 31) {
                    c.AND(X17, X16, 15);
                    c.CBNZ(X17, invalid);
                }
                if (reg_offset) {
                    load((w >> 16) & 31, X17);
                    unsigned option = (w >> 13) & 7;
                    if (option == 2)
                        c.UBFX(X17, X17, 0, 32);
                    if (option == 6)
                        c.SXTW(X17, W17);
                    c.ADD(X16, X16, X17, LSL, (w & (1U << 12)) ? size_log2 : 0);
                }
                if (mode != 1 && offset) {
                    if (offset > 4095) {
                        c.MOV(X17, uint64_t(offset));
                        c.ADD(X16, X16, X17);
                    } else if (offset > 0)
                        c.ADD(X16, X16, offset);
                    else
                        c.SUB(X16, X16, -offset);
                }
                // Retain tags in both the mapping key and architectural
                // writeback. The owner's MMU validates them before mapping RAM.
                if (size_log2 && (!unaligned || ordered)) {
                    c.AND(X17, X16, (1U << size_log2) - 1);
                    c.CBNZ(X17, invalid);
                }
                if (structure || pair || (unaligned && !ordered && size_log2)) {
                    c.ADD(X17, X16, access_bytes - 1);
                    c.EOR(X17, X17, X16);
                    c.LSR(X17, X17, page_bits);
                    c.CBNZ(X17, crossing);
                }
                translate(write, miss);
                unsigned data = vector || resident(rd) || rd == 31 ? rd : 26;
                if (structure) {
                    // Validate the complete list before issuing the host access.
                    // Guest writeback follows success and retains address tags.

                    c.dw((w & ~((1U << 23) | (31U << 16) | (31U << 5))) | (17 << 5));
                } else if (literal) {
                    unsigned opc = w >> 30;
                    uint32_t load_word = vector     ? (opc == 2 ? 0x3dc00000 : 0xbd400000 | (opc << 30))
                                         : opc == 2 ? 0xb9800000
                                                    : 0xb9400000 | (opc << 30);
                    c.dw(load_word | (17 << 5) | data);
                    if (!vector && data == 26)
                        save(rd, X26);
                } else if (atomic) {
                    unsigned rs = (w >> 16) & 31;
                    load(rs, X26);
                    load(rd, X30);

                    c.dw((w & ~((31U << 16) | 1023U)) | (26 << 16) | (17 << 5) | 30);
                    if ((w & 0x3fa07c00) == 0x08a07c00)
                        save(rs, X26);
                    else
                        save(rd, X30);
                } else if (pair) {
                    if (write && !vector && data == 26)
                        load(rd, X26);
                    unsigned rt2 = (w >> 10) & 31;
                    unsigned data2 = vector || resident(rt2) || rt2 == 31 ? rt2 : 30;
                    if (write && !vector && data2 == 30)
                        load(rt2, X30);
                    // Preserve a single architectural pair access after validating
                    // both elements, including their common page boundary.

                    c.dw((w & 0xc4400000) | 0x29000000 | (data2 << 10) | (17 << 5) | data);
                    if (!write && !vector) {
                        if (data == 26)
                            save(rd, X26);
                        if (data2 == 30)
                            save(rt2, X30);
                    }
                } else {
                    if (write && !vector && data == 26)
                        load(rd, X26);

                    c.dw(ordered ? (w & ~1023U) | (17 << 5) | data : (w & 0xc4c00000) | 0x39000000 | (17 << 5) | data);
                    if (!write && !vector && data == 26)
                        save(rd, X26);
                }
                if (mode) {
                    if (structure) {
                        unsigned rm = (w >> 16) & 31;
                        if (rm == 31)
                            c.ADD(X16, X16, access_bytes);
                        else {
                            load(rm, X17);
                            c.ADD(X16, X16, X17);
                        }
                    } else if (mode == 1 && offset) {
                        if (offset > 0)
                            c.ADD(X16, X16, offset);
                        else
                            c.SUB(X16, X16, -offset);
                    }
                    save(rn, X16, true);
                }
                c.B(done);
                c.l(miss);
                c.STR(X16, X27, DIRECT_ADDRESS);
                exit(pc, write ? WritePage : ReadPage);
                c.l(invalid);
                exit(pc, Alignment);
                c.l(crossing);
                exit(pc, CrossPage);
                c.l(done);
            } else {
                exit(pc, Fallback);
            }
            if (i + 1 < input.size() && !emit[i + 1])
                jump(pc + 4);
        }
        if (i % 32 == 31) {
            // TBNZ has a 32 KiB reach. Keep budget exits near their callers.
            Label next;
            c.B(next);
            for (size_t j = i - 31; j <= i; ++j) {
                if (emit[j] && leaders[j]) {
                    c.l(exhausted[j - first]);
                    exit(new_page + j * 4, Budget);
                }
            }
            c.l(next);
        }
    }
    c.l(labels[limit - first]);
    exit(new_page + 4096, CodeBranch);
    if (output.size() * 4 > max_bytes)
        throw std::length_error("direct code page exceeds pool");
    pthread_jit_write_protect_np(0);
    std::memcpy(executable, output.data(), output.size() * 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(executable, output.size() * 4);
    source = input;
    return output.size() * 4;
}
}  // namespace nce_direct
