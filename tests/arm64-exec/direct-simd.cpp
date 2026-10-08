/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)

struct Memory {
    alignas(16384) std::array<uint8_t, 16384> bytes{};
    bool readable = true, writable = true;
    static bool read(void *p, uint64_t a, void *v, size_t n) {
        if (a < 0x20000 || n > 16384 || a - 0x20000 > 16384 - n) return false;
        std::memcpy(v, static_cast<Memory *>(p)->bytes.data() + a - 0x20000, n);
        return true;
    }
    static bool write(void *p, uint64_t a, const void *v, size_t n) {
        if (a < 0x20000 || n > 16384 || a - 0x20000 > 16384 - n) return false;
        std::memcpy(static_cast<Memory *>(p)->bytes.data() + a - 0x20000, v, n);
        return true;
    }
    static void *map(void *p, uint64_t a, bool write) {
        auto &m = *static_cast<Memory *>(p);
        return a >= 0x20000 && a < 0x24000 && (write ? m.writable : m.readable) ?
            m.bytes.data() + a - 0x20000 : nullptr;
    }
};

int main()
{
    using Engine = std::unique_ptr<HuskArm64Exec, decltype(&husk_arm64_destroy)>;
    unsetenv("HUSK_ARM64_BACKEND");
    Engine reference(husk_arm64_create(), husk_arm64_destroy);
    setenv("HUSK_ARM64_BACKEND", "direct", 1);
    Engine direct(husk_arm64_create(), husk_arm64_destroy);
    CHECK(reference && direct);
    std::mt19937_64 random(0x53494d44);
    std::array<uint32_t, 1024> code{};
    auto compare = [&](HuskArm64State a, unsigned length, unsigned sample) {
        auto b = a;
        auto initial = a;
        Memory ma, mb;
        for (auto &byte : ma.bytes) byte = random();
        mb = ma;
        HuskArm64Memory ca{&ma, Memory::read, Memory::write, Memory::map};
        HuskArm64Memory cb{&mb, Memory::read, Memory::write, Memory::map};
        auto ra = husk_arm64_run(reference.get(), &a, 0x10000, code.data(), ca, 1024);
        auto rb = husk_arm64_run(direct.get(), &b, 0x10000, code.data(), cb, 1024);
        if (ra != HUSK_ARM64_FALLBACK || rb != ra || a.pc != 0x10000 + length * 4 ||
            std::memcmp(&a, &b, sizeof(a)) || ma.bytes != mb.bytes) {
            std::fprintf(stderr, "sample=%u pc=%llx/%llx nzcv=%x/%x fpsr=%x/%x\n", sample,
                (unsigned long long)a.pc, (unsigned long long)b.pc, a.nzcv, b.nzcv, a.fpsr, b.fpsr);
            for (unsigned r = 0; r < 32; ++r) {
                if (std::memcmp(a.v[r], b.v[r], 16))
                    std::fprintf(stderr, "v%u=%llx:%llx/%llx:%llx\n", r,
                        (unsigned long long)a.v[r][1], (unsigned long long)a.v[r][0],
                        (unsigned long long)b.v[r][1], (unsigned long long)b.v[r][0]);
                if (r < 31 && a.x[r] != b.x[r]) std::fprintf(stderr, "x%u=%llx/%llx\n", r,
                    (unsigned long long)a.x[r], (unsigned long long)b.x[r]);
            }
            for (unsigned i = 0; i < length; ++i) std::fprintf(stderr, "%08x\n", code[i]);
            auto previous = initial;
            for (unsigned end = 1; end <= length; ++end) {
                uint32_t saved = code[end];
                code[end] = 0xd4000001;
                auto x = initial, y = initial;
                husk_arm64_run(reference.get(), &x, 0x10000, code.data(), ca, 1024);
                husk_arm64_run(direct.get(), &y, 0x10000, code.data(), cb, 1024);
                code[end] = saved;
                if (std::memcmp(&x, &y, sizeof(x))) {
                    std::fprintf(stderr, "first difference after instruction %u (%08x), fpcr=%x fpsr=%x/%x\n",
                                 end - 1, code[end - 1], initial.fpcr, x.fpsr, y.fpsr);
                    for (unsigned r : {(code[end - 1] >> 5) & 31, (code[end - 1] >> 16) & 31})
                        std::fprintf(stderr, "input v%u=%016llx:%016llx\n", r,
                            (unsigned long long)previous.v[r][1], (unsigned long long)previous.v[r][0]);
                    break;
                }
                previous = x;
            }
            std::abort();
        }
    };
    auto state = [&] {
        HuskArm64State a{};
        for (auto &r : a.x) r = random();
        for (auto &v : a.v) { v[0] = random(); v[1] = random(); }
        a.pc = 0x10000;
        a.sp = 0x20040;
        a.nzcv = random() & 0xf0000000;
        a.fpcr = (random() & 15) << 22; // All four rounding modes, FZ and DN.
        a.fpsr = random() & 0x0800009f;
        return a;
    };

    for (unsigned sample = 0; sample < 2048; ++sample) {
        auto a = state();
        code.fill(0);
        for (unsigned i = 0; i < 64; ++i) {
            unsigned rd = i & 31, rn = random() & 31, rm = random() & 31;
            unsigned q = random() & 1, size = random() % (q ? 4 : 3);
            uint32_t operands = (rm << 16) | (rn << 5) | rd;
            switch ((sample + i) % 14) {
            case 0:
                code[i] = 0x0e201c00 | (q << 30) | ((random() & 1) << 29) |
                          ((random() & 3) << 22) | operands;
                break;
            case 1: {
                constexpr uint32_t ops[] = {0x0e208400, 0x2e208400, 0x0e20bc00,
                    0x0e203400, 0x0e203c00, 0x2e203400, 0x2e203c00, 0x2e208c00,
                    0x0e001800, 0x0e002800, 0x0e003800, 0x0e005800, 0x0e006800, 0x0e007800};
                auto op = ops[random() % std::size(ops)];
                // Dynarmic's ARM64 reference lacks VectorMaxU64, used by its
                // unsigned comparison lowering. Linux tests cover 64-bit lanes.
                if (size == 3 && (op == 0x2e203400 || op == 0x2e203c00)) size = 2;
                code[i] = op | (q << 30) | (size << 22) | operands;
                break;
            }
            case 2:
                code[i] = 0x2e000000 | (q << 30) | ((random() % (q ? 16 : 8)) << 11) | operands;
                break;
            case 3: {
                unsigned lane = random() % (16 >> size), imm5 = (lane << (size + 1)) | (1 << size);
                constexpr uint32_t ops[] = {0x0e000400, 0x0e000c00, 0x4e001c00};
                code[i] = ops[random() % 3] | (q << 30) | (imm5 << 16) | (rn << 5) | rd;
                break;
            }
            case 4: {
                unsigned lane = random() % (16 >> size), imm5 = (lane << (size + 1)) | (1 << size);
                code[i] = 0x0e003c00 | ((size == 3 ? 1 : 0) << 30) | (imm5 << 16) | (rn << 5) | rd;
                break;
            }
            case 5: {
                unsigned cmode = random() & 15, op = random() & 1;
                if (op && cmode == 15) q = 1;
                code[i] = 0x0f000400 | (q << 30) | (op << 29) | ((random() & 7) << 16) |
                          (cmode << 12) | ((random() & 31) << 5) | rd;
                break;
            }
            case 6: {
                constexpr uint32_t ops[] = {0x0e20cc00, 0x0ea0cc00, 0x0e20d400,
                                           0x0ea0d400, 0x2e20dc00, 0x2e20fc00};
                unsigned wide = random() & 1;
                auto op = ops[random() % 6];
                // The reference divides zero-filled inactive lanes for Q=0,
                // spuriously setting IOC. Linux checks Q=0 division and FPSR.
                if (op == 0x2e20fc00) q = 1;
                code[i] = op | ((q || wide) << 30) | (wide << 22) | operands;
                break;
            }
            case 7:
                code[i] = 0x1e200800 | ((random() & 1) << 22) | ((random() & 7) << 12) | operands;
                break;
            case 8: {
                constexpr uint32_t ops[] = {0x1e204000, 0x1e20c000, 0x1e214000,
                    0x1e21c000, 0x1e244000, 0x1e24c000, 0x1e254000, 0x1e25c000,
                    0x1e264000, 0x1e274000, 0x1e27c000};
                code[i] = ops[random() % std::size(ops)] | ((random() & 1) << 22) | (rn << 5) | rd;
                break;
            }
            case 9:
                code[i] = 0x1f000000 | ((random() & 1) << 22) | ((random() & 1) << 21) |
                          ((random() & 1) << 15) | ((random() & 31) << 10) | operands;
                break;
            case 10: {
                constexpr uint32_t ops[] = {0x1e270000, 0x9e670000, 0x9eaf0000,
                    0x1e260000, 0x9e660000, 0x9eae0000, 0x1e22c000, 0x1e624000};
                code[i] = ops[random() % std::size(ops)] | (rn << 5) | rd;
                break;
            }
            case 11: {
                constexpr uint32_t ops[] = {0x1e220000, 0x1e230000, 0x1e380000, 0x1e390000};
                code[i] = ops[random() % 4] | ((random() & 1) << 31) | ((random() & 1) << 22) | (rn << 5) | rd;
                break;
            }
            case 12: {
                constexpr uint32_t ops[] = {0x0e205800, 0x2e205800, 0x2e605800, 0x0e201800,
                    0x0e209800, 0x0e208800, 0x2e208800, 0x2e209800, 0x0e20a800,
                    0x0e20b800, 0x2e20b800, 0x0e204800, 0x2e204800, 0x0e200800, 0x2e200800};
                unsigned index = random() % std::size(ops);
                if (index < 4) size = 0;
                else if (index >= 11) size %= index == 14 ? 2 : 3;
                code[i] = ops[index] | (q << 30) | (size << 22) | (rn << 5) | rd;
                break;
            }
            case 13: {
                constexpr uint32_t ops[] = {0x0e20a400, 0x0e20ac00, 0x2e20a400, 0x2e20ac00};
                code[i] = ops[random() % 4] | (q << 30) | ((size % 3) << 22) | operands;
                break;
            }
            }
            CHECK(husk_arm64_supported(code[i]));
        }
        code[64] = 0xd4000001;
        compare(a, 64, sample);
    }
    // Loads and stores include all vector registers, base registers, pair sizes
    // and writeback modes. Compare the complete state and memory after each run.
    for (unsigned sample = 0; sample < 2048; ++sample) {
        auto a = state();
        unsigned rn = sample & 31, rt = random() & 31, rt2 = (rt + 1 + (random() % 31)) & 31;
        if (rn != 31) a.x[rn] = 0x20040;
        unsigned size = sample % 3, step = 32 >> (size + 2);
        code.fill(0);
        code[0] = (size << 30) | 0x2d800000 | (((-step) & 127) << 15) | (rt2 << 10) | (rn << 5) | rt;
        code[1] = 0x6e201c00 | (rt << 16) | (rt << 5) | rt;
        code[2] = 0x6e201c00 | (rt2 << 16) | (rt2 << 5) | rt2;
        code[3] = (size << 30) | 0x2cc00000 | (step << 15) | (rt2 << 10) | (rn << 5) | rt;
        code[4] = 0xd4000001;
        compare(a, 4, 10000 + sample);
    }
    // Interleaved and sequential lists include wrapping v31, all address
    // registers and post-index modes. Compare stores and loads with Dynarmic.
    constexpr unsigned structure_ops[] = {0, 2, 4, 6, 7, 8, 10};
    for (unsigned sample = 0; sample < 2048; ++sample) {
        auto a = state();
        unsigned op = structure_ops[sample % std::size(structure_ops)];
        unsigned rn = sample & 31, rt = random() & 31, q = random() & 1;
        if (rn == 31) q = 1; // Keep SP aligned after the first post-index access.
        bool sequential = op == 2 || op == 6 || op == 7 || op == 10;
        unsigned size = random() % (q || sequential ? 4 : 3);
        unsigned rm = sample % 3 ? 31 : (rn + 1) % 31;
        bool post = sample & 1;
        if (rm != 31) a.x[rm] = 64;
        if (rn != 31) a.x[rn] = 0x20040;
        code.fill(0);
        code[0] = 0x0c000000 | (q << 30) | (post << 23) | ((post ? rm : 0) << 16) |
                  (op << 12) | (size << 10) | (rn << 5) | rt;
        code[1] = code[0] | (1U << 22);
        code[2] = 0xd4000001;
        CHECK(husk_arm64_supported(code[0]) && husk_arm64_supported(code[1]));
        compare(a, 2, 20000 + sample);
    }
    for (unsigned sample = 0; sample < 6; ++sample) {
        auto a = state();
        Memory memory;
        memory.readable = sample != 0;
        memory.writable = sample != 1;
        a.x[0] = sample == 2 ? 0x20ff0 : sample == 3 ? 0x20041 : 0x20040;
        if (sample == 4) a.access_flags = 2;
        code.fill(0);
        code[0] = (sample == 1 ? 0x4c9fa800 : 0x4cdfa800); // st1/ld1 {v0.4s,v1.4s},[x0],#32
        if (sample == 5) { a.sp = 0x20048; code[0] |= 31 << 5; }
        auto before = a;
        auto bytes = memory.bytes;
        HuskArm64Memory callbacks{&memory, Memory::read, Memory::write, Memory::map};
        CHECK(husk_arm64_run(direct.get(), &a, 0x10000, code.data(), callbacks, 1024) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&a, &before, sizeof(a)) && memory.bytes == bytes);
    }
    uint64_t old_fpcr, old_fpsr;
    asm volatile("mrs %0, fpcr\nmrs %1, fpsr" : "=r"(old_fpcr), "=r"(old_fpsr));
    uint64_t host_fpcr = 0x01c00000, host_fpsr = 0x08000001;
    asm volatile("msr fpcr, %0\nmsr fpsr, %1" : : "r"(host_fpcr), "r"(host_fpsr));
    auto a = state();
    code.fill(0);
    code[0] = 0x1e201800; // fdiv s0,s0,s0
    code[1] = 0xd4000001;
    Memory m;
    HuskArm64Memory callbacks{&m, Memory::read, Memory::write, Memory::map};
    auto result = husk_arm64_run(direct.get(), &a, 0x10000, code.data(), callbacks, 1024);
    uint64_t after_fpcr, after_fpsr;
    asm volatile("mrs %0, fpcr\nmrs %1, fpsr" : "=r"(after_fpcr), "=r"(after_fpsr));
    asm volatile("msr fpcr, %0\nmsr fpsr, %1" : : "r"(old_fpcr), "r"(old_fpsr));
    CHECK(result == HUSK_ARM64_FALLBACK && host_fpcr == after_fpcr && host_fpsr == after_fpsr);
    for (uint32_t word : {0x0e228400U | (3U << 22), 0x1ee02800U, 0x0e000c00U,
                          0x0f00f400U | (1U << 29), 0x7dc00000U, 0xec400440U,
                          0x4c403000U, 0x4c417000U, 0x0c408c00U, 0x0ee09800U,
                          0x6ee2ac20U, 0x4ee05800U})
        CHECK(!husk_arm64_supported(word));
    std::puts("PASS: 131072 SIMD/FP instructions, 4096 vector memory cases, all vectors/GPRs, FPCR/FPSR, host FP environment");
}
