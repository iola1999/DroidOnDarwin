/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"
#include "husk-arm64-direct.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed line %d\n", __LINE__); std::exit(1); } } while (0)

struct Memory {
    std::array<unsigned char, 131072> bytes{};
    int denied_read = -1, denied_write = -1;
    unsigned assists = 0;
    static void *Map(void *opaque, uint64_t address, bool write) {
        auto &m = *static_cast<Memory *>(opaque);
        if (address >= 0x30000 && address < 0x50000)
            return m.bytes.data() + (31 - (address - 0x30000) / 4096) * 4096;
        if (address < 0x10000 || address >= 0x30000) return nullptr;
        unsigned page = (address - 0x10000) / 4096;
        if (int(page) == (write ? m.denied_write : m.denied_read)) return nullptr;
        return m.bytes.data() + address - 0x10000;
    }
    static HuskArm64Assist Assist(void *opaque, HuskArm64State *state, uint32_t instruction) {
        if (instruction != HUSK_BULK_INSN) return HUSK_ASSIST_DECLINED;
        auto &m = *static_cast<Memory *>(opaque);
        ++m.assists;
        state->x[8] = !husk_arm64_bulk(state->x[0], state->x[1], state->x[2], state->x[3],
                                      {opaque, nullptr, nullptr, Map});
        state->pc += 4;
        return HUSK_ASSIST_CONTINUE;
    }
};

int main()
{
    Memory memory;
    HuskArm64Memory callbacks{&memory, nullptr, nullptr, Memory::Map};
    std::mt19937_64 random(0x42554c4b);
    for (unsigned op = 0; op < 3; ++op) {
        for (unsigned sample = 0; sample < 128; ++sample) {
            for (auto &v : memory.bytes) v = random();
            auto expected = memory.bytes;
            size_t n = sample == 0 ? 65536 : random() % 65537;
            size_t from = random() % (memory.bytes.size() - n);
            size_t to = random() % (memory.bytes.size() - n);
            if (op == HUSK_BULK_SET) std::memset(expected.data() + to, from & 255, n);
            else std::memmove(expected.data() + to, expected.data() + from, n);
            CHECK(husk_arm64_bulk(0x10000 + to, op == HUSK_BULK_SET ? from : 0x10000 + from, n, op, callbacks));
            CHECK(memory.bytes == expected);
        }
    }
    auto original = memory.bytes;
    memory.denied_write = 15;
    CHECK(!husk_arm64_bulk(0x10000, 0x20000, 65536, HUSK_BULK_COPY, callbacks));
    CHECK(memory.bytes == original);
    memory.denied_write = -1; memory.denied_read = 31;
    CHECK(!husk_arm64_bulk(0x10000, 0x20000, 65536, HUSK_BULK_MOVE, callbacks));
    CHECK(memory.bytes == original);
    CHECK(!husk_arm64_bulk(UINT64_MAX - 4, 0, 8, HUSK_BULK_SET, callbacks));
    CHECK(!husk_arm64_bulk(0x10000, 0, 65537, HUSK_BULK_SET, callbacks));
    CHECK(!husk_arm64_bulk(0x10000, 0, 16, 99, callbacks));
    CHECK(memory.bytes == original);
    CHECK(husk_arm64_bulk(UINT64_MAX, UINT64_MAX, 0, HUSK_BULK_COPY, {}));
    memory.denied_read = -1;
    auto aliased = memory.bytes;
    for (size_t i = 0; i < 65536; ++i)
        aliased[128 + i] = memory.bytes[(31 - i / 4096) * 4096 + i % 4096];
    CHECK(husk_arm64_bulk(0x10080, 0x30000, 65536, HUSK_BULK_MOVE, callbacks));
    CHECK(memory.bytes == aliased);

    std::array<uint32_t, 1024> code;
    code.fill(0xd4000001);
    code[0] = HUSK_BULK_INSN;
    code[1] = 0x17ffffff; // b to the preceding bulk request
    HuskArm64State state{};
    for (unsigned i = 0; i < 31; ++i) state.x[i] = 0x12340000 + i;
    for (unsigned i = 0; i < 32; ++i) { state.v[i][0] = 0xabc000 + i; state.v[i][1] = 0xdef000 + i; }
    state.x[0] = 0x10000; state.x[1] = 0x20000; state.x[2] = 65536; state.x[3] = 0;
    state.pc = 0x80000; state.sp = 0x22000; state.nzcv = 0xb0000000; state.fpsr = 0x08000091;
    auto expected = state;
    expected.pc += 4; expected.x[8] = 0;
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(husk_direct_create(), husk_direct_destroy);
    CHECK(husk_direct_run(engine.get(), &state, 0x80000, code.data(),
                         {&memory, nullptr, nullptr, Memory::Map, Memory::Assist}, 256) == HUSK_ARM64_BUDGET);
    CHECK(memory.assists == 1 && !std::memcmp(&state, &expected, sizeof(state)));
    std::puts("bulk copy/move/set, overlap, bounds and refusal without partial writes PASS");
    std::puts("Direct bulk assist preserves complete guest state and returns on its byte budget PASS");
}
