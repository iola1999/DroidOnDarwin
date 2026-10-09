/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-direct.h"
#include <array>
#include <cstdio>
#include <exception>

/* The iOS archive contains the direct backend; Dynarmic remains a Mac comparator. */
bool husk_arm64_supported(uint32_t instruction)
{
    return husk_direct_supported(instruction);
}

HuskArm64Exec *husk_arm64_create()
{
    HuskArm64Direct *ctx = nullptr;
    try {
        ctx = husk_direct_create();
        std::array<uint32_t, 1024> code{};
        code[0] = 0xd2800001;
        code[1] = 0x8b000021;
        code[2] = 0xf1000400;
        code[3] = 0x54ffffc1;
        code[4] = 0xd4000001;
        HuskArm64State state{};
        state.x[0] = 100;
        state.pc = 0x10000;
        if (husk_direct_run(ctx, &state, 0x10000, code.data(), {}, 10000) != HUSK_ARM64_FALLBACK ||
            state.x[1] != 5050 || state.x[0] || state.pc != 0x10010 || state.nzcv != 0x60000000) {
            husk_direct_destroy(ctx);
            std::fprintf(stderr, "[husk-arm64] direct self-test failed\n");
            return nullptr;
        }
        std::fprintf(stderr, "[husk-arm64] backend=direct-ios self-test PASS sum=5050\n");
        return reinterpret_cast<HuskArm64Exec *>(ctx);
    } catch (const std::exception &error) {
        husk_direct_destroy(ctx);
        std::fprintf(stderr, "[husk-arm64] create: %s\n", error.what());
        return nullptr;
    }
}

void husk_arm64_destroy(HuskArm64Exec *ctx)
{
    husk_direct_destroy(reinterpret_cast<HuskArm64Direct *>(ctx));
}

HuskArm64Exit husk_arm64_run(HuskArm64Exec *ctx, HuskArm64State *state, uint64_t page,
                             const void *bytes, HuskArm64Memory memory, uint64_t budget)
{
    if (!ctx || !state || !bytes || !budget || (page & 4095) || page > UINT64_MAX - 4095)
        return HUSK_ARM64_ERROR;
    try {
        return husk_direct_run(reinterpret_cast<HuskArm64Direct *>(ctx), state, page, bytes, memory, budget);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[husk-arm64] execution: %s\n", error.what());
        return HUSK_ARM64_ERROR;
    }
}
