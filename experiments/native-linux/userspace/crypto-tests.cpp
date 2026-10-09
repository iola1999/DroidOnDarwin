/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <openssl/aead.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "crypto-test:%d: %s\n", __LINE__, #c); _exit(110); } } while (0)

void nce_crypto_tests()
{
    CHECK(FIPS_mode() == 0 && BORINGSSL_self_test_all() == 1);
    const unsigned char expected_sha[] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };
    unsigned char digest[SHA256_DIGEST_LENGTH];
    CHECK(SHA256(reinterpret_cast<const unsigned char *>("abc"), 3, digest));
    CHECK(!memcmp(digest, expected_sha, sizeof(digest)));
    /* Pinned BoringSSL aes_128_gcm_tests.txt: zero key, nonce and 16-byte plaintext. */
    const unsigned char expected_gcm[] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78,
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf,
    };
    unsigned char key[16] = {}, nonce[12] = {}, plain[16] = {}, sealed[32], opened[16];
    size_t sealed_size, opened_size;
    EVP_AEAD_CTX *ctx = EVP_AEAD_CTX_new(EVP_aead_aes_128_gcm(), key, sizeof(key), 16);
    CHECK(ctx);
    CHECK(EVP_AEAD_CTX_seal(ctx, sealed, &sealed_size, sizeof(sealed), nonce, sizeof(nonce),
                          plain, sizeof(plain), nullptr, 0) == 1);
    CHECK(sealed_size == sizeof(expected_gcm) && !memcmp(sealed, expected_gcm, sealed_size));
    CHECK(EVP_AEAD_CTX_open(ctx, opened, &opened_size, sizeof(opened), nonce, sizeof(nonce),
                          sealed, sealed_size, nullptr, 0) == 1);
    CHECK(opened_size == sizeof(plain) && !memcmp(opened, plain, opened_size));
    sealed[sizeof(sealed) - 1] ^= 1;
    CHECK(EVP_AEAD_CTX_open(ctx, opened, &opened_size, sizeof(opened), nonce, sizeof(nonce),
                          sealed, sealed_size, nullptr, 0) == 0);
    const size_t bytes = 1024 * 1024;
    unsigned char *input = static_cast<unsigned char *>(calloc(bytes, 1));
    unsigned char *output = static_cast<unsigned char *>(malloc(bytes + 16));
    unsigned char *roundtrip = static_cast<unsigned char *>(malloc(bytes));
    CHECK(input && output && roundtrip);
    timespec start, finish;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (unsigned i = 0; i < 64; ++i) {
        nonce[0] = i;
        CHECK(EVP_AEAD_CTX_seal(ctx, output, &sealed_size, bytes + 16, nonce, sizeof(nonce),
                              input, bytes, nullptr, 0) == 1 && sealed_size == bytes + 16);
    }
    CHECK(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    CHECK(EVP_AEAD_CTX_open(ctx, roundtrip, &opened_size, bytes, nonce, sizeof(nonce),
                          output, sealed_size, nullptr, 0) == 1);
    CHECK(opened_size == bytes && !memcmp(input, roundtrip, bytes));
    double elapsed = (finish.tv_sec - start.tv_sec) * 1000.0 + (finish.tv_nsec - start.tv_nsec) / 1e6;
    printf("{\"benchmark\":\"android_aes_gcm\",\"hwcap\":%lu,\"bytes\":%zu,\"elapsed_ms\":%.3f}\n",
           getauxval(AT_HWCAP), bytes * 64, elapsed);
    free(roundtrip); free(output); free(input);
    EVP_AEAD_CTX_free(ctx);
    puts("{\"test\":\"android_crypto\",\"passed\":true,\"fips\":false,\"known_answers\":true,\"reject_bad_tag\":true}");
}
