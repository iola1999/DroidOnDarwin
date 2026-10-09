/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"
#include "husk-arm64-direct.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <memory>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::fprintf(stderr, "failed line %d\n", __LINE__); \
            std::exit(1); \
        } \
    } while (0)

struct Memory {
    std::array<unsigned char, 131072> bytes{};
    int denied_read = -1, denied_write = -1;
    unsigned assists = 0;
    static void *Map(void *opaque, uint64_t address, bool write)
    {
        auto &m = *static_cast<Memory *>(opaque);
        if (address >= 0x110000 && address < 0x130000)
            address -= 0x100000;
        if (address >= 0x30000 && address < 0x50000)
            return m.bytes.data() + (31 - (address - 0x30000) / 4096) * 4096;
        if (address < 0x10000 || address >= 0x30000)
            return nullptr;
        unsigned page = (address - 0x10000) / 4096;
        if (int(page) == (write ? m.denied_write : m.denied_read))
            return nullptr;
        return m.bytes.data() + address - 0x10000;
    }
    static HuskArm64Assist Assist(void *opaque, HuskArm64State *state, uint32_t instruction)
    {
        if (husk_arm64_mops_prologue(instruction)) {
            ++static_cast<Memory *>(opaque)->assists;
            uint32_t stage = (instruction & 0xffe0fc00) == 0x19c00400 ? 0x4000 : 0x400000;
            uint32_t code[] = {instruction, instruction | stage, instruction | (stage * 2)};
            return husk_arm64_mops(state, code, {opaque, nullptr, nullptr, Map}) ? HUSK_ASSIST_CONTINUE : HUSK_ASSIST_DECLINED;
        }
        if (instruction != HUSK_BULK_INSN)
            return HUSK_ASSIST_DECLINED;
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
            for (auto &v : memory.bytes)
                v = random();
            auto expected = memory.bytes;
            size_t n = sample == 0 ? 65536 : random() % 65537;
            size_t from = random() % (memory.bytes.size() - n);
            size_t to = random() % (memory.bytes.size() - n);
            if (op == HUSK_BULK_SET)
                std::memset(expected.data() + to, from & 255, n);
            else
                std::memmove(expected.data() + to, expected.data() + from, n);
            CHECK(husk_arm64_bulk(0x10000 + to, op == HUSK_BULK_SET ? from : 0x10000 + from, n, op, callbacks));
            CHECK(memory.bytes == expected);
        }
    }
    auto original = memory.bytes;
    memory.denied_write = 15;
    CHECK(!husk_arm64_bulk(0x10000, 0x20000, 65536, HUSK_BULK_COPY, callbacks));
    CHECK(memory.bytes == original);
    memory.denied_write = -1;
    memory.denied_read = 31;
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

    for (unsigned set = 0; set < 2; ++set)
        for (unsigned sample = 0; sample < 128; ++sample) {
            for (auto &v : memory.bytes)
                v = random();
            auto expected_memory = memory.bytes;
            uint64_t length = sample == 0 ? 0 : sample == 1 ? 65536
                                                            : random() % 65537;
            uint64_t to = random() % (memory.bytes.size() - length);
            uint64_t from = random() % (memory.bytes.size() - length);
            if (set)
                std::memset(expected_memory.data() + to, from & 255, length);
            else
                std::memmove(expected_memory.data() + to, expected_memory.data() + from, length);
            uint32_t word = (set ? 0x19c00400 : 0x1d000400) | (1 << 16) | (2 << 5);
            uint32_t stage = set ? 0x4000 : 0x400000;
            uint32_t words[] = {word, word | stage, word | (stage * 2)};
            HuskArm64State state{};
            for (auto &x : state.x)
                x = random();
            state.pc = 0x80000;
            state.nzcv = 0xb0000000;
            state.x[0] = 0x10000 + to;
            state.x[1] = set ? from : 0x10000 + from;
            state.x[2] = length;
            if (sample & 1) {
                state.access_flags |= 8;
                state.x[0] |= UINT64_C(0xab00000000000000);
                if (!set)
                    state.x[1] |= UINT64_C(0xcd00000000000000);
            }
            auto expected_state = state;
            expected_state.x[2] = 0;
            expected_state.pc += 12;
            expected_state.nzcv = 0;
            if (set || !(from < to && to - from < length)) {
                expected_state.x[0] += length;
                if (!set)
                    expected_state.x[1] += length;
            }
            CHECK(husk_arm64_mops(&state, words, callbacks));
            CHECK(memory.bytes == expected_memory && !std::memcmp(&state, &expected_state, sizeof(state)));
        }
    uint32_t mops_words[] = {0x1d010440, 0x1d410440, 0x1d810440};
    for (unsigned failure = 0; failure < 5; ++failure) {
        HuskArm64State state{};
        state.pc = 0x80000;
        state.x[0] = 0x10000;
        state.x[1] = 0x20000;
        state.x[2] = 65536;
        auto words = std::array{mops_words[0], mops_words[1], mops_words[2]};
        if (failure == 0)
            memory.denied_write = 15;
        if (failure == 1)
            state.x[1] = 0x40000;  // reverse physical aliases overlap destination
        if (failure == 2)
            words[1] ^= 1;  // middle instruction uses a different destination
        if (failure == 3)
            state.x[2] = 65537;
        if (failure == 4)
            state.x[0] |= UINT64_C(0xab00000000000000);
        auto before = state;
        auto before_memory = memory.bytes;
        CHECK(!husk_arm64_mops(&state, words.data(), callbacks));
        CHECK(!std::memcmp(&state, &before, sizeof(state)) && memory.bytes == before_memory);
        memory.denied_write = -1;
    }

    std::array<uint32_t, 1024> code;
    code.fill(0xd4000001);
    code[0] = HUSK_BULK_INSN;
    code[1] = 0x17ffffff;  // b to the preceding bulk request
    HuskArm64State state{};
    for (unsigned i = 0; i < 31; ++i)
        state.x[i] = 0x12340000 + i;
    for (unsigned i = 0; i < 32; ++i) {
        state.v[i][0] = 0xabc000 + i;
        state.v[i][1] = 0xdef000 + i;
    }
    state.x[0] = 0x10000;
    state.x[1] = 0x20000;
    state.x[2] = 65536;
    state.x[3] = 0;
    state.pc = 0x80000;
    state.sp = 0x22000;
    state.nzcv = 0xb0000000;
    state.fpsr = 0x08000091;
    auto expected = state;
    expected.pc += 4;
    expected.x[8] = 0;
    std::unique_ptr<HuskArm64Direct, decltype(&husk_direct_destroy)> engine(husk_direct_create(), husk_direct_destroy);
    CHECK(husk_direct_run(engine.get(), &state, 0x80000, code.data(),
                          {&memory, nullptr, nullptr, Memory::Map, Memory::Assist}, 256) == HUSK_ARM64_BUDGET);
    CHECK(memory.assists == 1 && !std::memcmp(&state, &expected, sizeof(state)));
    std::copy(std::begin(mops_words), std::end(mops_words), code.begin());
    code[3] = 0xd4000001;
    state = {};
    state.pc = 0x80000;
    state.x[0] = 0x10000;
    state.x[1] = 0x20000;
    state.x[2] = 65536;
    memory.assists = 0;
    CHECK(husk_direct_run(engine.get(), &state, 0x80000, code.data(),
                          {&memory, nullptr, nullptr, Memory::Map, Memory::Assist}, 256) == HUSK_ARM64_BUDGET);
    CHECK(memory.assists == 1 && state.pc == 0x8000c && state.x[0] == 0x20000 &&
          state.x[1] == 0x30000 && !state.x[2] && !state.nzcv);

    // Native MOPS must preserve every unrelated register and copy overlapping
    // ranges without exporting state through the instruction-assist callback.
    for (bool set : {false, true})
        for (unsigned length = 1; length <= 64; ++length) {
            for (unsigned variant = 0; variant < 8; ++variant) {
                unsigned rd = random() % 31, rn, rs;
                do {
                    rn = random() % 31;
                } while (rn == rd);
                do {
                    rs = random() % (set ? 32 : 31);
                } while (rs == rd || rs == rn);
                uint32_t word = (set ? 0x19c00400 : 0x1d000400) | (rs << 16) | (rn << 5) | rd;
                uint32_t stage = set ? 0x4000 : 0x400000;
                code.fill(0xd4000001);
                code[0] = word;
                code[1] = word | stage;
                code[2] = word | (2 * stage);
                for (auto &v : memory.bytes)
                    v = random();
                auto ram_expected = memory.bytes;
                size_t from = 128 + random() % 2048;
                size_t to = variant < 4 ? from + variant - 1 : 4096 + random() % (4097 - length);
                uint64_t fill = rs == 31 ? 0 : random();
                state = {};
                for (auto &v : state.x)
                    v = random();
                for (auto &v : state.v) {
                    v[0] = random();
                    v[1] = random();
                }
                state.pc = 0x80000;
                state.sp = random();
                state.nzcv = 0xb0000000;
                state.fpcr = 0x03000000;
                state.fpsr = 0x08000091;
                state.tpidr_el0 = random();
                state.access_flags = HUSK_MOPS_ALLOWED;
                state.x[rd] = 0x10000 + to;
                state.x[rn] = length;
                if (rs != 31)
                    state.x[rs] = set ? fill : 0x10000 + from;
                if (variant & 1) {
                    state.access_flags |= 8;
                    state.x[rd] |= UINT64_C(0xab00000000000000);
                    if (!set)
                        state.x[rs] |= UINT64_C(0xcd00000000000000);
                }
                expected = state;
                bool reverse = !set && from < to && to - from < length;
                if (!reverse) {
                    expected.x[rd] += length;
                    if (!set)
                        expected.x[rs] += length;
                }
                expected.x[rn] = 0;
                expected.nzcv = 0;
                expected.pc += 12;
                if (set)
                    std::memset(ram_expected.data() + to, fill & 255, length);
                else
                    std::memmove(ram_expected.data() + to, ram_expected.data() + from, length);
                CHECK(husk_direct_run(engine.get(), &state, 0x80000, code.data(), callbacks, 4096) == HUSK_ARM64_FALLBACK);
                CHECK(!std::memcmp(&state, &expected, sizeof(state)) && memory.bytes == ram_expected);
            }
        }
    // Feature changes reuse one engine. Rejected cases must leave both memory
    // and architectural state intact, including flags changed by a later success.
    for (unsigned failure = 0; failure < 9; ++failure) {
        std::copy(std::begin(mops_words), std::end(mops_words), code.begin());
        state = {};
        state.pc = 0x80000;
        state.nzcv = 0xa0000000;
        state.access_flags = HUSK_MOPS_ALLOWED;
        state.x[0] = 0x10000;
        state.x[1] = 0x11000;
        state.x[2] = 64;
        if (failure == 0)
            state.access_flags = 0;
        if (failure == 1)
            state.x[0] |= UINT64_C(1) << 48;
        if (failure == 2)
            state.x[1] |= UINT64_C(1) << 48;
        if (failure == 3)
            state.x[0] += 4095;
        if (failure == 4)
            state.x[1] += 4095;
        if (failure == 5)
            memory.denied_write = 0;
        if (failure == 6)
            memory.denied_read = 1;
        if (failure == 7)
            code[1] ^= 1;
        if (failure == 8)
            state.x[2] = 65;
        auto before = state;
        auto ram_before = memory.bytes;
        CHECK(husk_direct_run(engine.get(), &state, 0x80000, code.data(), callbacks, 4096) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&state, &before, sizeof(state)) && memory.bytes == ram_before);
        memory.denied_write = memory.denied_read = -1;
    }
    // Read and write entries with the same hash slot retain separate tags.
    std::copy(std::begin(mops_words), std::end(mops_words), code.begin());
    state = {};
    state.pc = 0x80000;
    state.access_flags = HUSK_MOPS_ALLOWED;
    state.x[0] = 0x10000;
    state.x[1] = 0x111000;
    state.x[2] = 64;
    auto ram_expected = memory.bytes;
    std::memmove(ram_expected.data(), ram_expected.data() + 4096, 64);
    CHECK(husk_direct_run(engine.get(), &state, 0x80000, code.data(), callbacks, 4096) == HUSK_ARM64_FALLBACK);
    CHECK(state.pc == 0x8000c && state.x[0] == 0x10040 && state.x[1] == 0x111040 && !state.x[2]);
    CHECK(memory.bytes == ram_expected);
    std::puts("bulk copy/move/set, overlap, bounds and refusal without partial writes PASS");
    std::puts("Direct bulk assist preserves complete guest state and returns on its byte budget PASS");
    std::puts("native MOPS lengths 1..64, arbitrary registers, overlap and access gating PASS");
}
