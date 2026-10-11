/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "decode.h"
namespace nce_direct
{
int64_t sign_extend(uint32_t value, unsigned bits)
{
    return int64_t(uint64_t(value) << (64 - bits)) >> (64 - bits);
}
unsigned structure_registers(uint32_t word)
{
    switch ((word >> 12) & 15) {
        case 0:
        case 2:
            return 4;
        case 4:
        case 6:
            return 3;
        case 7:
            return 1;
        case 8:
        case 10:
            return 2;
        default:
            return 0;
    }
}
MemoryAccess memory_access(uint32_t word, Kind kind)
{
    bool vector = word & 0x04000000;
    bool pair = kind == Pair, literal = kind == Literal;
    bool ordered = kind == Atomic || (word & 0x3fbffc00) == 0x089ffc00;
    unsigned shift = pair ? 2 + (vector ? word >> 30 : word >> 31)
                          : (word >> 30) + (vector ? ((word >> 23) & 1) * 4 : 0);
    unsigned count = pair ? 2 : 1;
    if (literal)
        shift = 2 + (vector ? word >> 30 : (word >> 30) & 1);
    if (kind == StructureLane) {
        unsigned scale = (word >> 14) & 3, size = (word >> 10) & 3;
        shift = scale == 3 ? size : scale == 2 && (size & 1) ? 3
                                                             : scale;
        count = (((word >> 13) & 1) * 2 + ((word >> 21) & 1)) + 1;
    }
    unsigned bytes = count << shift;
    if (kind == Structure) {
        shift = (word >> 10) & 3;
        bytes = structure_registers(word) * (word & (1U << 30) ? 16 : 8);
    }
    bool write = !literal && (kind == Atomic || (pair || vector || ordered ? !(word & (1U << 22)) : ((word >> 22) & 3) == 0));
    return {shift, bytes, write};
}

Kind decode(uint32_t w)
{
    bool sf = w >> 31;
    if ((w & 0xffffffe0) == 0xd53b00e0)
        return Dczid;
    if ((w & 0xffffffe0) == 0xd50b7420)
        return CacheZero;
    if ((w & 0xffc00000) == 0xf9800000)
        return Hint;
    if ((w & 0x1f800000) == 0x11000000)
        return AddImm;
    if ((w & 0x1f800000) == 0x12800000 && ((w >> 29) & 3) != 1 &&
        (sf || !(w & (1U << 22))))
        return MoveWide;
    if ((w & 0x1f200000) == 0x0b000000 && ((w >> 22) & 3) != 3 &&
        (sf || !(w & (1U << 15))))
        return AddReg;
    if ((w & 0x1f000000) == 0x0a000000 &&
        (sf || !(w & (1U << 15))))
        return LogicalReg;
    if ((w & 0x1f800000) == 0x12000000 && (sf || !(w & (1U << 22)))) {
        unsigned imms = (w >> 10) & 63;
        unsigned mask = (((w >> 22) & 1) << 6) | ((~imms) & 63);
        if (mask > 1) {
            unsigned len = 31 - __builtin_clz(mask);
            if ((imms & ((1U << len) - 1)) != ((1U << len) - 1))
                return LogicalImm;
        }
    }
    if ((w & 0x1f800000) == 0x13000000 && ((w >> 29) & 3) != 3 &&
        ((w >> 22) & 1) == sf && (sf || !(w & ((1U << 21) | (1U << 15)))))
        return Bitfield;
    if ((w & 0x7fa00000) == 0x13800000 && ((w >> 22) & 1) == sf &&
        (sf || !(w & (1U << 15))))
        return Extract;
    switch (w & 0x7ffffc00) {
        case 0x5ac00000:
        case 0x5ac00400:
        case 0x5ac00800:
        case 0x5ac01000:
        case 0x5ac01400:
            return Unary;
    }
    if ((w & 0xfffffc00) == 0xdac00c00)
        return Unary;
    switch (w & 0x7fe0fc00) {
        case 0x1ac00800:
        case 0x1ac00c00:
        case 0x1ac02000:
        case 0x1ac02400:
        case 0x1ac02800:
        case 0x1ac02c00:
            return Binary;
    }
    switch (w & 0xffe0fc00) {
        case 0x1ac04000:
        case 0x1ac04400:
        case 0x1ac04800:
        case 0x9ac04c00:
        case 0x1ac05000:
        case 0x1ac05400:
        case 0x1ac05800:
        case 0x9ac05c00:
            return Crc;
    }
    if ((w & 0x3fe00800) == 0x1a800000)
        return Select;
    if ((w & 0x7fe00000) == 0x1b000000)
        return Multiply;
    if ((w & 0xffe00000) == 0x9b200000 || (w & 0xffe00000) == 0x9ba00000 ||
        (w & 0xffe0fc00) == 0x9b407c00 || (w & 0xffe0fc00) == 0x9bc07c00)
        return Multiply;
    if ((w & 0x1fe00000) == 0x0b200000 && ((w >> 10) & 7) <= 4)
        return AddExt;
    if ((w & 0x1fe0fc00) == 0x1a000000)
        return Carry;
    if ((w & 0x3fe00410) == 0x3a400000)
        return Compare;
    if (w == 0xd503201f)
        return Hint;
    if ((w & 0xfffff0ff) == 0xd503305f)
        return ClearExclusive;
    switch (w & 0xffffffe0) {
        case 0xd53bd040:
        case 0xd53b4200:
        case 0xd53b4400:
        case 0xd53b4420:
            return SystemRead;
    }
    if ((w & 0x1f000000) == 0x10000000)
        return Adr;
    if ((w & 0x7c000000) == 0x14000000)
        return Branch;
    if ((w & 0xff000010) == 0x54000000)
        return Conditional;
    if ((w & 0x7e000000) == 0x34000000)
        return CompareBranch;
    if ((w & 0x7e000000) == 0x36000000)
        return TestBranch;
    auto br = w & 0xfffffc1f;
    if (br == 0xd61f0000 || br == 0xd63f0000)
        return Indirect;
    if (br == 0xd65f0000)
        return Indirect;
    switch (w) {
        case 0xd50320ff:
        case 0xd503211f:
        case 0xd503215f:
        case 0xd503219f:
        case 0xd50321df:
        case 0xd503231f:
        case 0xd503233f:
        case 0xd503235f:
        case 0xd503237f:
        case 0xd503239f:
        case 0xd50323bf:
        case 0xd50323df:
        case 0xd50323ff:
            return Pauth;
        case 0xd503241f:
        case 0xd503245f:
        case 0xd503249f:
        case 0xd50324df:
            return Bti;
    }
    if ((w & 0xfffff0ff) == 0xd50330bf || (w & 0xfffff0ff) == 0xd503309f)
        return Barrier;
    bool vector = w & 0x04000000;
    if ((w & 0x3b000000) == 0x18000000) {
        if ((w >> 30) < 3)
            return Literal;
        return vector ? Other : Hint;
    }
    if ((w & 0x3fff7c00) == 0x085f7c00)
        return ExclusiveLoad;
    if ((w & 0x3fe07c00) == 0x08007c00)
        return ExclusiveStore;
    if ((w & 0x3fa07c00) == 0x08a07c00 ||
        ((w & 0x3f200c00) == 0x38200000 && ((w >> 12) & 15) <= 8))
        return Atomic;
    if ((w & 0x3fbffc00) == 0x089ffc00)
        return Memory;  // LDAR/STLR, all integer sizes
    if ((w & 0x3a000000) == 0x28000000 &&
        (vector ? (w >> 30) < 3 : ((w >> 30) & 1) == 0)) {
        unsigned rn = (w >> 5) & 31, rt = w & 31, rt2 = (w >> 10) & 31;
        bool writeback = (w >> 23) & 1;
        if (((w >> 22) & 1) && rt == rt2)
            return Other;
        if (!vector && writeback && rn != 31 && (rn == rt || rn == rt2))
            return Other;
        return Pair;
    }
    unsigned opc = (w >> 22) & 3, size = w >> 30;
    bool scalar_memory = opc <= 1 || (opc == 2 && size < 3) || (opc == 3 && size < 2);
    if (vector ? size + ((opc >> 1) * 4) <= 4 : scalar_memory) {
        if ((w & 0x3b000000) == 0x39000000)
            return Memory;
        if ((w & 0x3b200c00) == 0x38200800 && (w & (1U << 14)))
            return Memory;
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
        case 0x4e284800:
        case 0x4e285800:
        case 0x4e286800:
        case 0x4e287800:
            return CryptoAes;
        case 0x5e280800:
        case 0x5e281800:
            return CryptoSha1;
        case 0x5e282800:
            return CryptoSha256;
    }
    switch (w & 0xffe0fc00) {
        case 0x5e000000:
        case 0x5e001000:
        case 0x5e002000:
        case 0x5e003000:
            return CryptoSha1;
        case 0x5e004000:
        case 0x5e005000:
        case 0x5e006000:
            return CryptoSha256;
    }
    if ((w & 0xbf200000) == 0x0c000000 && structure_registers(w)) {
        unsigned opcode = (w >> 12) & 15;
        bool sequential = opcode == 2 || opcode == 6 || opcode == 7 || opcode == 10;
        if (((w & (1U << 23)) || !((w >> 16) & 31)) &&
            (((w >> 10) & 3) != 3 || q || sequential))
            return Structure;
    }
    if ((w & 0xbf000000) == 0x0d000000 &&
        ((w & (1U << 23)) || !((w >> 16) & 31))) {
        unsigned scale = (w >> 14) & 3, size = (w >> 10) & 3;
        bool s = w & (1U << 12);
        if (scale == 0 || (scale == 1 && !(size & 1)) ||
            (scale == 2 && !(size & 2) && (!(size & 1) || !s)) ||
            (scale == 3 && (w & (1U << 22)) && !s))
            return StructureLane;
    }
    if ((w & 0x9ff80c00) == 0x0f000400 &&
        (q || !((w >> 29) & 1) || ((w >> 12) & 15) != 15))
        return Vector;
    unsigned immh = (w >> 19) & 15;
    if ((w & 0x9f800400) == 0x0f000400 && immh) {
        unsigned opcode = (w >> 11) & 31;
        bool unsigned_op = w & (1U << 29);
        // Narrow/widen forms use Q to select a half, including Q=0 32-bit lanes.
        if (opcode >= 16 && opcode <= 20 && !(immh & 8))
            return Vector;
        if (q || !(immh & 8)) {
            switch (opcode) {
                case 0:
                case 2:
                case 4:
                case 6:
                case 10:
                case 14:
                    return Vector;
                case 8:
                case 12:
                    if (unsigned_op)
                        return Vector;
                    break;
            }
        }
    }
    if ((w & 0x9f20fc00) == 0x0e201c00)
        return Vector;  // AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF
    switch (w & 0xbf20fc00) {
        case 0x0e001800:
        case 0x0e002800:
        case 0x0e003800:
        case 0x0e005800:
        case 0x0e006800:
        case 0x0e007800:
        case 0x0e208400:
        case 0x2e208400:
        case 0x0e20bc00:
        case 0x0e203400:
        case 0x0e203c00:
        case 0x2e203400:
        case 0x2e203c00:
        case 0x2e208c00:
        case 0x0e208c00:
        case 0x0e204400:
        case 0x2e204400:  // SSHL/USHL
        case 0x0e204c00:
        case 0x2e204c00:  // SQSHL/UQSHL
        case 0x0e205400:
        case 0x2e205400:  // SRSHL/URSHL
            if (element != 3 || q)
                return Vector;
            break;
        case 0x0e209c00:
        case 0x0e209400:
        case 0x2e209400:
        case 0x0e206400:
        case 0x0e206c00:
        case 0x2e206400:
        case 0x2e206c00:
        case 0x0e20a400:
        case 0x0e20ac00:
        case 0x2e20a400:
        case 0x2e20ac00:
        case 0x0e20c000:
        case 0x2e20c000:  // SMULL/UMULL
        case 0x0e208000:
        case 0x2e208000:  // SMLAL/UMLAL
        case 0x0e20a000:
        case 0x2e20a000:  // SMLSL/UMLSL
        case 0x0e201000:
        case 0x2e201000:  // SADDW/UADDW
        case 0x0e203000:
        case 0x2e203000:  // SSUBW/USUBW
            if (element != 3)
                return Vector;
            break;
    }
    switch (w & 0xbffffc00) {
        case 0x0e205800:
        case 0x2e205800:
        case 0x2e605800:  // CNT/NOT/RBIT
        case 0x0e201800:
            return Vector;  // REV16
    }
    switch (w & 0xbf3ffc00) {
        case 0x0e209800:
        case 0x0e208800:
        case 0x2e208800:
        case 0x2e209800:
        case 0x0e20a800:  // signed comparisons with zero
        case 0x0e20b800:
        case 0x2e20b800:  // ABS/NEG
            if (element != 3 || q)
                return Vector;
            break;
        case 0x0e204800:
        case 0x2e204800:  // CLS/CLZ
        case 0x0e200800:  // REV64
            if (element != 3)
                return Vector;
            break;
        case 0x2e200800:
            if (element < 2)
                return Vector;
            break;  // REV32
        case 0x0e303800:
        case 0x2e303800:  // SADDLV/UADDLV
        case 0x0e30a800:
        case 0x2e30a800:  // SMAXV/UMAXV
        case 0x0e31a800:
        case 0x2e31a800:
        case 0x0e31b800:  // SMINV/UMINV/ADDV
            if (element < 2 || (element == 2 && q))
                return Vector;
            break;
        case 0x0e202800:
        case 0x2e202800:  // SADDLP/UADDLP
        case 0x0e206800:
        case 0x2e206800:  // SADALP/UADALP
        case 0x0e212800:
        case 0x2e212800:  // XTN/SQXTUN
        case 0x0e214800:
        case 0x2e214800:  // SQXTN/UQXTN
        case 0x2e213800:  // SHLL
            if (element < 3)
                return Vector;
            break;
    }
    if ((w & 0xbfe08400) == 0x2e000000 && (q || !(w & (1U << 14))))
        return Vector;
    unsigned imm5 = (w >> 16) & 31;
    if (imm5) {
        unsigned lane_size = __builtin_ctz(imm5);
        if (lane_size < 4) {
            switch (w & 0xbfe0fc00) {
                case 0x0e000400:
                    if (lane_size < 3 || q)
                        return Vector;
                    break;
                case 0x0e000c00:
                    if (lane_size < 3 || q)
                        return VectorFromGpr;
                    break;
                case 0x0e002c00:
                    if (lane_size < (q ? 3U : 2U))
                        return GprFromVector;
                    break;
                case 0x0e003c00:
                    if (q ? lane_size == 3 : lane_size < 3)
                        return GprFromVector;
                    break;
            }
            if ((w & 0xffe0fc00) == 0x4e001c00)
                return VectorFromGpr;
            if ((w & 0xffe08400) == 0x6e000400 &&
                !(((w >> 11) & 15) & ((1U << lane_size) - 1)))
                return Vector;
        }
    }
    // Only validated FP32/FP64 encodings use the host FPCR/FPSR.
    if ((w & 0xffbffc00) == 0x5e21d800 || (w & 0xffbffc00) == 0x7e21d800)
        return Vector;  // scalar SCVTF/UCVTF from a SIMD register
    if (!((w >> 22) & 1) || q) {
        switch (w & 0xbfa0fc00) {
            case 0x0e20cc00:
            case 0x0ea0cc00:
            case 0x0e20d400:
            case 0x0ea0d400:
            case 0x2e20dc00:
            case 0x2e20fc00:
                return Vector;
        }
    }
    if (element <= 1) {
        if ((w & 0x7f200000) == 0x1e000000 && (sf || (w & (1U << 15)))) {
            unsigned opcode = (w >> 16) & 31;
            if (opcode == 2 || opcode == 3)
                return FpFromGpr;
            if (opcode == 24 || opcode == 25)
                return GprFromFp;
        }
        switch (w & 0xff3ffc00) {
            case 0x1e204000:
            case 0x1e20c000:
            case 0x1e214000:
            case 0x1e21c000:
            case 0x1e244000:
            case 0x1e24c000:
            case 0x1e254000:
            case 0x1e25c000:
            case 0x1e264000:
            case 0x1e274000:
            case 0x1e27c000:
                return Vector;
        }
        switch (w & 0xff20fc00) {
            case 0x1e200800:
            case 0x1e201800:
            case 0x1e202800:
            case 0x1e203800:
            case 0x1e204800:
            case 0x1e205800:
            case 0x1e206800:
            case 0x1e207800:
                return Vector;
        }
        if ((w & 0xff200c00) == 0x1e200c00 ||  // FCSEL
            (w & 0xff201fe0) == 0x1e201000 ||  // FMOV immediate
            (w & 0xff000000) == 0x1f000000)
            return Vector;  // FMADD/FMSUB/FNMADD/FNMSUB
        if ((w & 0xff20fc07) == 0x1e202000 &&
            (!(w & 8) || !((w >> 16) & 31)))
            return Vector;  // FCMP/FCMPE
        if ((w & 0x7f3ffc00) == 0x1e220000 ||
            (w & 0x7f3ffc00) == 0x1e230000)
            return FpFromGpr;  // SCVTF/UCVTF
        switch (w & 0x7f3ffc00) {
            case 0x1e200000:
            case 0x1e210000:
            case 0x1e240000:
            case 0x1e250000:
            case 0x1e280000:
            case 0x1e290000:
            case 0x1e300000:
            case 0x1e310000:
            case 0x1e380000:
            case 0x1e390000:
                return GprFromFp;  // FCVTN/A/P/M/Z, signed and unsigned
        }
    }
    switch (w & 0xfffffc00) {
        case 0x1e270000:
        case 0x9e670000:
        case 0x9eaf0000:
            return FpFromGpr;
        case 0x1e260000:
        case 0x9e660000:
        case 0x9eae0000:
            return GprFromFp;
        case 0x1e22c000:
        case 0x1e624000:
            return Vector;  // FCVT S/D
    }
    return Other;
}
bool integer_kind(Kind kind)
{
    return kind >= AddImm && kind <= Carry;
}
uint64_t target(uint32_t w, Kind kind, uint64_t pc)
{
    if (kind == Branch)
        return pc + sign_extend(w & 0x3ffffff, 26) * 4;
    unsigned bits = kind == TestBranch ? 14 : 19;
    return pc + sign_extend((w >> 5) & ((1U << bits) - 1), bits) * 4;
}

}  // namespace nce_direct
