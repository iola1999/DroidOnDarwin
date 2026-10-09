/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-exec.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); \
            std::abort(); \
        } \
    } while (0)

struct Memory {
    alignas(16384) std::array<unsigned char, 16384> bytes{};
    static bool read(void *p, uint64_t a, void *out, size_t n)
    {
        if (a < 0x20000 || n > 16384 || a - 0x20000 > 16384 - n)
            return false;
        std::memcpy(out, static_cast<Memory *>(p)->bytes.data() + a - 0x20000, n);
        return true;
    }
    static bool write(void *p, uint64_t a, const void *in, size_t n)
    {
        if (a < 0x20000 || n > 16384 || a - 0x20000 > 16384 - n)
            return false;
        std::memcpy(static_cast<Memory *>(p)->bytes.data() + a - 0x20000, in, n);
        return true;
    }
    static void *map(void *p, uint64_t a, bool)
    {
        return a >= 0x20000 && a < 0x24000 ? static_cast<Memory *>(p)->bytes.data() + a - 0x20000 : nullptr;
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
    std::mt19937_64 random(0x4845534b);
    std::array<uint32_t, 1024> code{};
    HuskArm64Memory no_memory{
        nullptr, [](void *, uint64_t, void *, size_t) { return false; },
        [](void *, uint64_t, const void *, size_t) { return false; }, nullptr};
    // Each register, including Darwin-reserved x18 and guest SP, appears as a
    // source and destination. Random streams additionally exercise dependencies.
    for (unsigned sample = 0; sample < 1024; ++sample) {
        HuskArm64State a{};
        for (auto &r : a.x)
            r = random();
        a.sp = random() & ~15ULL;
        a.pc = 0x10000;
        a.nzcv = uint32_t(random()) & 0xf0000000;
        a.access_flags = 64;
        for (unsigned i = 0; i < 64; ++i) {
            unsigned rd = i & 31, rn = (i + sample) & 31, rm = random() & 31;
            unsigned sf = random() & 1, op = random() & 3;
            switch ((i + sample) % 17) {
                case 0:
                    code[i] = (sf << 31) | (op << 29) | 0x11000000 |
                              ((random() & 4095) << 10) | (rn << 5) | rd;
                    break;
                case 1:
                    code[i] = (sf << 31) | (op << 29) | 0x0b000000 |
                              ((random() % 3) << 22) | (rm << 16) |
                              ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 2:
                    code[i] = (sf << 31) | (op << 29) | 0x0a000000 |
                              ((random() & 7) << 21) | (rm << 16) |
                              ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 3:
                    code[i] = (sf << 31) | ((op == 1 ? 2 : op) << 29) | 0x12800000 |
                              ((random() & (sf ? 3 : 1)) << 21) |
                              ((random() & 65535) << 5) | rd;
                    break;
                case 4:
                    code[i] = (sf << 31) | (op << 29) | 0x12000000 |
                              (sf << 22) | ((random() & (sf ? 63 : 31)) << 16) |
                              ((random() % (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 5:
                    code[i] = (sf << 31) | ((op % 3) << 29) | 0x13000000 |
                              (sf << 22) | ((random() & (sf ? 63 : 31)) << 16) |
                              ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 6:
                    code[i] = (sf << 31) | 0x13800000 | (sf << 22) | (rm << 16) |
                              ((random() & (sf ? 63 : 31)) << 10) | (rn << 5) | rd;
                    break;
                case 7: {
                    constexpr uint32_t ops[] = {0x5ac00000, 0x5ac00400, 0x5ac00800, 0x5ac01000, 0x5ac01400};
                    code[i] = (sf << 31) | ops[random() % 5] | (rn << 5) | rd;
                    break;
                }
                case 8: {
                    constexpr uint32_t ops[] = {0x1ac00800, 0x1ac00c00, 0x1ac02000, 0x1ac02400, 0x1ac02800, 0x1ac02c00};
                    code[i] = (sf << 31) | ops[random() % 6] | (rm << 16) | (rn << 5) | rd;
                    break;
                }
                case 9:
                    code[i] = (sf << 31) | ((random() & 1) << 30) | 0x1a800000 |
                              ((random() & 1) << 10) | ((random() & 15) << 12) |
                              (rm << 16) | (rn << 5) | rd;
                    break;
                case 10:
                    code[i] = (sf << 31) | 0x1b000000 | ((random() & 1) << 15) |
                              ((random() & 31) << 10) | (rm << 16) | (rn << 5) | rd;
                    break;
                case 11:
                    code[i] = 0xdac00c00 | (rn << 5) | rd;
                    break;
                case 12:
                    code[i] = (sf << 31) | 0x0b200000 | (op << 29) | (rm << 16) |
                              ((random() & 7) << 13) | ((random() % 5) << 10) | (rn << 5) | rd;
                    break;
                case 13:
                    code[i] = (sf << 31) | (op << 29) | 0x1a000000 |
                              (rm << 16) | (rn << 5) | rd;
                    break;
                case 14:
                    code[i] = (sf << 31) | ((random() & 1) << 30) | 0x3a400000 |
                              ((random() & 1) << 11) | ((random() & 15) << 12) |
                              (rm << 16) | (rn << 5) | (random() & 15);
                    break;
                case 15: {
                    constexpr uint32_t ops[] = {0x9b207c00, 0x9ba07c00, 0x9b407c00, 0x9bc07c00};
                    code[i] = ops[random() % 4] | (rm << 16) | (rn << 5) | rd;
                    break;
                }
                case 16: {
                    constexpr uint32_t ops[] = {0x1ac04000, 0x1ac04400, 0x1ac04800, 0x9ac04c00,
                                                0x1ac05000, 0x1ac05400, 0x1ac05800, 0x9ac05c00};
                    code[i] = ops[random() % 8] | (rm << 16) | (rn << 5) | rd;
                    break;
                }
            }
        }
        code[64] = 0xd4000001;
        HuskArm64State b = a;
        auto ra = husk_arm64_run(reference.get(), &a, 0x10000, code.data(), no_memory, 1000);
        auto rb = husk_arm64_run(direct.get(), &b, 0x10000, code.data(), no_memory, 1000);
        if (ra != rb || a.pc != b.pc || a.sp != b.sp || a.nzcv != b.nzcv ||
            std::memcmp(a.x, b.x, sizeof(a.x))) {
            std::fprintf(stderr, "sample=%u reference_pc=%llx direct_pc=%llx\n", sample,
                         (unsigned long long)a.pc, (unsigned long long)b.pc);
            for (unsigned r = 0; r < 31; ++r)
                if (a.x[r] != b.x[r])
                    std::fprintf(stderr, "x%u: %llx / %llx\n", r,
                                 (unsigned long long)a.x[r], (unsigned long long)b.x[r]);
            std::abort();
        }
    }
    for (unsigned base = 0; base < 31; ++base) {
        for (unsigned data = 0; data < 31; ++data) {
            if (data == base)
                continue;
            unsigned counter = 0;
            while (counter == base || counter == data)
                ++counter;
            code.fill(0);
            code[0] = 0xf9400000 | (base << 5) | data;
            code[1] = 0x91000400 | (data << 5) | data;
            code[2] = 0xf9000000 | (base << 5) | data;
            code[3] = 0xf1000400 | (counter << 5) | counter;
            code[4] = 0x54ffff81;
            code[5] = 0xd4000001;
            HuskArm64State a{};
            for (auto &r : a.x)
                r = random();
            a.pc = 0x10000;
            a.x[base] = 0x20000;
            a.x[counter] = 20;
            a.nzcv = random() & 0xf0000000;
            HuskArm64State b = a;
            Memory ma, mb;
            HuskArm64Memory ca{&ma, Memory::read, Memory::write, Memory::map};
            HuskArm64Memory cb{&mb, Memory::read, Memory::write, Memory::map};
            CHECK(husk_arm64_run(reference.get(), &a, 0x10000, code.data(), ca, 1000) == HUSK_ARM64_FALLBACK);
            CHECK(husk_arm64_run(direct.get(), &b, 0x10000, code.data(), cb, 1000) == HUSK_ARM64_FALLBACK);
            CHECK(a.pc == b.pc && a.sp == b.sp && a.nzcv == b.nzcv);
            CHECK(!std::memcmp(a.x, b.x, sizeof(a.x)) && ma.bytes == mb.bytes);
        }
    }
    for (unsigned sample = 0; sample < 1024; ++sample) {
        unsigned rn = sample & 31, rt = random() % 31, rt2 = random() % 31;
        if (rn == rt || rn == rt2 || rt == rt2)
            continue;
        unsigned wide = (sample >> 5) & 1;
        unsigned scaled = wide ? 2 : 4;
        code.fill(0);
        code[0] = (wide << 31) | 0x29800000 | (((-scaled) & 127) << 15) |
                  (rt2 << 10) | (rn << 5) | rt;  // stp, pre-index -16
        code[1] = 0xd2800000 | rt;
        code[2] = 0xd2800000 | rt2;
        code[3] = (wide << 31) | 0x28c00000 | (scaled << 15) |
                  (rt2 << 10) | (rn << 5) | rt;  // ldp, post-index +16
        code[4] = 0xd4000001;
        HuskArm64State a{};
        for (auto &r : a.x)
            r = random();
        a.sp = 0x20020;
        if (rn != 31)
            a.x[rn] = 0x20020;
        a.pc = 0x10000;
        a.nzcv = random() & 0xf0000000;
        auto b = a;
        Memory ma, mb;
        HuskArm64Memory ca{&ma, Memory::read, Memory::write, Memory::map};
        HuskArm64Memory cb{&mb, Memory::read, Memory::write, Memory::map};
        CHECK(husk_arm64_run(reference.get(), &a, 0x10000, code.data(), ca, 100) == HUSK_ARM64_FALLBACK);
        CHECK(husk_arm64_run(direct.get(), &b, 0x10000, code.data(), cb, 100) == HUSK_ARM64_FALLBACK);
        CHECK(a.pc == b.pc && a.sp == b.sp && a.nzcv == b.nzcv);
        CHECK(!std::memcmp(a.x, b.x, sizeof(a.x)) && ma.bytes == mb.bytes);
    }
    for (unsigned sample = 0; sample < 1024; ++sample) {
        unsigned size = sample & 3, option = std::array{2U, 3U, 6U, 7U}[(sample >> 2) & 3];
        unsigned scaled = (sample >> 4) & 1;
        unsigned rn = (sample >> 5) & 31, rm = random() % 31, rt = random() % 31;
        if (rn == rm || rn == rt || rm == rt)
            continue;
        code.fill(0);
        code[0] = (size << 30) | 0x38200800 | (rm << 16) | (option << 13) |
                  (scaled << 12) | (rn << 5) | rt;
        code[1] = 0xd2800000 | rt;
        code[2] = code[0] | (1U << 22);
        code[3] = 0xd4000001;
        HuskArm64State a{};
        for (auto &r : a.x)
            r = random();
        a.sp = 0x20040;
        if (rn != 31)
            a.x[rn] = 0x20040;
        a.x[rm] = option >= 6 ? UINT64_MAX - 7 : 8;
        if (option == 2 || option == 6)
            a.x[rm] = (random() << 32) | uint32_t(a.x[rm]);
        a.pc = 0x10000;
        a.nzcv = random() & 0xf0000000;
        auto b = a;
        Memory ma, mb;
        HuskArm64Memory ca{&ma, Memory::read, Memory::write, Memory::map};
        HuskArm64Memory cb{&mb, Memory::read, Memory::write, Memory::map};
        CHECK(husk_arm64_run(reference.get(), &a, 0x10000, code.data(), ca, 100) == HUSK_ARM64_FALLBACK);
        CHECK(husk_arm64_run(direct.get(), &b, 0x10000, code.data(), cb, 100) == HUSK_ARM64_FALLBACK);
        CHECK(a.pc == b.pc && a.sp == b.sp && a.nzcv == b.nzcv);
        CHECK(!std::memcmp(a.x, b.x, sizeof(a.x)) && ma.bytes == mb.bytes);
    }
    std::puts("PASS: 65536 differential integer instructions, all GPRs, SP, NZCV");
    std::puts("PASS: 930 mapped-loop register combinations and guest-memory results");
    std::puts("PASS: pair writeback, register offsets, signed indices, guest-memory results");
    for (unsigned sample = 0; sample < 2048; ++sample) {
        unsigned rn = sample & 31, rt = random() & 31, size = (sample >> 5) & 3;
        if (rn == rt && rn != 31)
            continue;
        code.fill(0);
        if (sample & 1) {
            code[0] = (size << 30) | 0x089ffc00 | (rn << 5) | rt;  // STLR
            code[1] = 0xd2800000 | rt;
            code[2] = code[0] | (1U << 22);  // LDAR
            code[3] = 0xd4000001;
        } else {
            size %= 3;
            unsigned opc = size < 2 && (sample & 64) ? 3 : 2;
            code[0] = (size << 30) | 0x38000400 | (opc << 22) | (16 << 12) | (rn << 5) | rt;
            code[1] = 0xd4000001;
        }
        HuskArm64State a{};
        for (auto &r : a.x)
            r = random();
        a.sp = 0x20040;
        if (rn != 31)
            a.x[rn] = a.sp;
        a.pc = 0x10000;
        a.nzcv = random() & 0xf0000000;
        auto b = a;
        Memory ma, mb;
        for (auto &value : ma.bytes)
            value = random();
        mb = ma;
        HuskArm64Memory ca{&ma, Memory::read, Memory::write, Memory::map};
        HuskArm64Memory cb{&mb, Memory::read, Memory::write, Memory::map};
        CHECK(husk_arm64_run(reference.get(), &a, 0x10000, code.data(), ca, 100) == HUSK_ARM64_FALLBACK);
        CHECK(husk_arm64_run(direct.get(), &b, 0x10000, code.data(), cb, 100) == HUSK_ARM64_FALLBACK);
        CHECK(!std::memcmp(&a, &b, sizeof(a)) && ma.bytes == mb.bytes);
    }
    std::puts("PASS: signed loads, acquire/release memory, NZCV preservation");
}
