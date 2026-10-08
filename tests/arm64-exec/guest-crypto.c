/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <arm_neon.h>

static const uint32_t round_constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void compress(uint32x4_t *state0, uint32x4_t *state1, uint32x4_t message[4])
{
    uint32x4_t a = *state0, b = *state1;
    for (unsigned round = 0; round < 16; ++round) {
        unsigned slot = round & 3;
        uint32x4_t wk = vaddq_u32(message[slot], vld1q_u32(round_constants + round * 4));
        uint32x4_t old_a = a;
        a = vsha256hq_u32(a, b, wk);
        b = vsha256h2q_u32(b, old_a, wk);
        if (round < 12) {
            message[slot] = vsha256su0q_u32(message[slot], message[(slot + 1) & 3]);
            message[slot] = vsha256su1q_u32(message[slot], message[(slot + 2) & 3],
                                          message[(slot + 3) & 3]);
        }
    }
    *state0 = vaddq_u32(*state0, a);
    *state1 = vaddq_u32(*state1, b);
}

/* Hash count complete blocks of 'a', including SHA-256 padding. */
void husk_sha256_blocks(unsigned count, unsigned digest[8])
{
    uint32x4_t state0 = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a};
    uint32x4_t state1 = {0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (unsigned i = 0; i < count; ++i) {
        uint32x4_t msg[4] = {vdupq_n_u32(0x61616161), vdupq_n_u32(0x61616161),
                            vdupq_n_u32(0x61616161), vdupq_n_u32(0x61616161)};
        compress(&state0, &state1, msg);
    }
    uint64_t bits = (uint64_t)count * 512;
    uint32x4_t padding[4] = {{0x80000000, 0, 0, 0}, {0}, {0},
                             {0, 0, (uint32_t)(bits >> 32), (uint32_t)bits}};
    compress(&state0, &state1, padding);
    vst1q_u32(digest, state0);
    vst1q_u32(digest + 4, state1);
}
