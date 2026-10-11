/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "internal.h"
#include <bit>
#include <cstring>

namespace nce_direct
{
void transfer_cross_page(uint32_t word, Kind kind, MemoryAccess access,
                         NceDirectState &state, unsigned char *bytes, uint64_t address)
{
    static_assert(std::endian::native == std::endian::little);
    unsigned rd = word & 31, rn = (word >> 5) & 31;
    unsigned element = 1U << access.element_shift;
    bool vector = word & 0x04000000;
    auto reg = [&](unsigned index, bool sp = false) {
        return index == 31 ? (sp ? state.sp : UINT64_C(0)) : state.x[index];
    };
    auto transfer = [&](void *value, unsigned offset, unsigned length) {
        if (access.write)
            std::memcpy(bytes + offset, value, length);
        else
            std::memcpy(value, bytes + offset, length);
    };
    if (kind == StructureLane) {
        unsigned scale = (word >> 14) & 3;
        unsigned count = access.bytes / element;
        unsigned vector_bytes = word & (1U << 30) ? 16 : 8;
        unsigned lane = (((word >> 30) & 1) * 8 + ((word >> 12) & 1) * 4 + ((word >> 10) & 3)) >> access.element_shift;
        for (unsigned index = 0; index < count; ++index) {
            auto *data = reinterpret_cast<unsigned char *>(state.v[(rd + index) & 31]);
            if (scale == 3) {
                std::memset(data, 0, 16);
                for (unsigned offset = 0; offset < vector_bytes; offset += element)
                    std::memcpy(data + offset, bytes + index * element, element);
            } else
                transfer(data + lane * element, index * element, element);
        }
    } else if (kind == Structure) {
        unsigned count = structure_registers(word), opcode = (word >> 12) & 15;
        unsigned vector_bytes = access.bytes / count;
        bool sequential = opcode == 2 || opcode == 6 || opcode == 7 || opcode == 10;
        for (unsigned index = 0; index < count; ++index) {
            auto *data = reinterpret_cast<unsigned char *>(state.v[(rd + index) & 31]);
            if (!access.write)
                std::memset(data, 0, 16);
            for (unsigned offset = 0; offset < vector_bytes; offset += element) {
                unsigned source = sequential ? index * vector_bytes + offset : offset * count + index * element;
                transfer(data + offset, source, element);
            }
        }
    } else {
        unsigned count = kind == Pair ? 2 : 1;
        for (unsigned index = 0; index < count; ++index) {
            unsigned target = index ? (word >> 10) & 31 : rd;
            if (vector) {
                if (!access.write)
                    std::memset(state.v[target], 0, 16);
                transfer(state.v[target], index * element, element);
            } else {
                uint64_t value = reg(target);
                if (!access.write)
                    value = 0;
                transfer(&value, index * element, element);
                if (!access.write && target != 31) {
                    unsigned opc = kind == Literal ? word >> 30 : kind == Pair ? 1
                                                                               : (word >> 22) & 3;
                    if (opc >= 2)
                        value = sign_extend(uint32_t(value), element * 8);
                    if (opc == 3)
                        value = uint32_t(value);
                    state.x[target] = value;
                }
            }
        }
    }
    bool structure = kind == Structure || kind == StructureLane;
    bool indexed = kind == Memory && (word & 0x3b200000) == 0x38000000;
    unsigned mode = structure ? (word >> 23) & 1 : kind == Pair ? ((word >> 23) & 1 ? (word >> 23) & 3 : 0)
                                               : indexed        ? (word >> 10) & 3
                                                                : 0;
    if (mode) {
        uint64_t next = address;
        if (structure) {
            unsigned rm = (word >> 16) & 31;
            next += rm == 31 ? access.bytes : reg(rm);
        } else if (mode == 1) {
            next += kind == Pair ? sign_extend((word >> 15) & 127, 7) * element : sign_extend((word >> 12) & 511, 9);
        }
        if (rn == 31)
            state.sp = next;
        else
            state.x[rn] = next;
    }
}
}  // namespace nce_direct
