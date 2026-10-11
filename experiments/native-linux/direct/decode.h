/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
namespace nce_direct
{
enum Kind { Other,
            AddImm,
            MoveWide,
            AddReg,
            LogicalReg,
            LogicalImm,
            Bitfield,
            Extract,
            Unary,
            Binary,
            Crc,
            Select,
            Multiply,
            AddExt,
            Carry,
            Adr,
            Branch,
            Conditional,
            CompareBranch,
            TestBranch,
            Indirect,
            Memory,
            Pair,
            Vector,
            VectorTable,
            CryptoAes,
            CryptoSha1,
            CryptoSha256,
            VectorFromGpr,
            GprFromVector,
            FpFromGpr,
            GprFromFp,
            Compare,
            Hint,
            SystemRead,
            Atomic,
            Pauth,
            Barrier,
            Bti,
            ExclusiveLoad,
            ExclusiveStore,
            ClearExclusive,
            Structure,
            StructureLane,
            Literal,
            Dczid,
            CacheZero };

unsigned structure_registers(uint32_t);
struct MemoryAccess {
    unsigned element_shift, bytes;
    bool write;
};
MemoryAccess memory_access(uint32_t, Kind);
Kind decode(uint32_t);
bool integer_kind(Kind);
uint64_t target(uint32_t, Kind, uint64_t);
int64_t sign_extend(uint32_t, unsigned);
}  // namespace nce_direct
