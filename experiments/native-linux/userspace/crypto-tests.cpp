/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dlfcn.h>
#include <openssl/aead.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "crypto-test:%d: %s\n", __LINE__, #c); \
            _exit(110); \
        } \
    } while (0)

template <typename T>
static T crypto_symbol(void *library, const char *name)
{
    auto symbol = reinterpret_cast<T>(dlsym(library, name));
    CHECK(symbol);
    return symbol;
}

void nce_crypto_tests()
{
    // SDK clients allocate these public contexts using the Android 16 ABI.
    static_assert(sizeof(EVP_MD_CTX) == 32 && sizeof(HMAC_CTX) == 104 && sizeof(EVP_AEAD_CTX) == 584);
    const int fips = FIPS_mode();
    const int self_test = BORINGSSL_self_test();
    printf("{\"android_crypto_runtime\":true,\"fips_mode\":%d,\"self_test\":%d}\n", fips, self_test);
    CHECK((fips == 0 || fips == 1) && self_test == 1);
    struct {
        HMAC_CTX context;
        unsigned char guard[32];
    } hmac;
    memset(hmac.guard, 0xa5, sizeof(hmac.guard));
    HMAC_CTX_init(&hmac.context);
    unsigned char hmac_key[20], mac[32];
    memset(hmac_key, 0x0b, sizeof(hmac_key));
    // RFC 4231 test case 1, split across two streaming updates.
    const unsigned char expected_mac[] = {
        0xb0,
        0x34,
        0x4c,
        0x61,
        0xd8,
        0xdb,
        0x38,
        0x53,
        0x5c,
        0xa8,
        0xaf,
        0xce,
        0xaf,
        0x0b,
        0xf1,
        0x2b,
        0x88,
        0x1d,
        0xc2,
        0x00,
        0xc9,
        0x83,
        0x3d,
        0xa7,
        0x26,
        0xe9,
        0x37,
        0x6c,
        0x2e,
        0x32,
        0xcf,
        0xf7,
    };
    unsigned mac_size = 0;
    CHECK(HMAC_Init_ex(&hmac.context, hmac_key, sizeof(hmac_key), EVP_sha256(), nullptr) == 1);
    CHECK(HMAC_Update(&hmac.context, reinterpret_cast<const unsigned char *>("Hi "), 3) == 1);
    CHECK(HMAC_Update(&hmac.context, reinterpret_cast<const unsigned char *>("There"), 5) == 1);
    CHECK(HMAC_Final(&hmac.context, mac, &mac_size) == 1);
    HMAC_CTX_cleanup(&hmac.context);
    CHECK(mac_size == sizeof(expected_mac) && !memcmp(mac, expected_mac, mac_size));
    for (unsigned char byte : hmac.guard)
        CHECK(byte == 0xa5);
    puts("{\"test\":\"android_crypto_sdk_context_abi\",\"passed\":true,\"hmac_context_bytes\":104,\"aead_context_bytes\":584}");
    // Cronet carries a separately versioned BoringSSL with the SDK context ABI.
    void *cronet = dlopen("stable_cronet_libcrypto.so", RTLD_NOW | RTLD_LOCAL);
    CHECK(cronet);
    const int cronet_fips = crypto_symbol<decltype(&FIPS_mode)>(cronet, "FIPS_mode")();
    CHECK(cronet_fips == 0 || cronet_fips == 1);
    CHECK(crypto_symbol<decltype(&BORINGSSL_self_test)>(cronet, "BORINGSSL_self_test")() == 1);
    memset(hmac.guard, 0xa5, sizeof(hmac.guard));
    crypto_symbol<decltype(&HMAC_CTX_init)>(cronet, "HMAC_CTX_init")(&hmac.context);
    auto cronet_sha256 = crypto_symbol<decltype(&EVP_sha256)>(cronet, "EVP_sha256")();
    CHECK(crypto_symbol<decltype(&HMAC_Init_ex)>(cronet, "HMAC_Init_ex")(&hmac.context, hmac_key, sizeof(hmac_key), cronet_sha256, nullptr) == 1);
    CHECK(crypto_symbol<decltype(&HMAC_Update)>(cronet, "HMAC_Update")(&hmac.context, reinterpret_cast<const unsigned char *>("Hi There"), 8) == 1);
    CHECK(crypto_symbol<decltype(&HMAC_Final)>(cronet, "HMAC_Final")(&hmac.context, mac, &mac_size) == 1);
    crypto_symbol<decltype(&HMAC_CTX_cleanup)>(cronet, "HMAC_CTX_cleanup")(&hmac.context);
    CHECK(mac_size == sizeof(expected_mac) && !memcmp(mac, expected_mac, mac_size));
    for (unsigned char byte : hmac.guard)
        CHECK(byte == 0xa5);
    // BoringSSL TLS destructors can run when the process exits; retain the library.
    printf("{\"test\":\"android_cronet_crypto\",\"passed\":true,\"fips\":%s,\"hmac_context_bytes\":104}\n", cronet_fips ? "true" : "false");
    const unsigned char expected_sha[] = {
        0xba,
        0x78,
        0x16,
        0xbf,
        0x8f,
        0x01,
        0xcf,
        0xea,
        0x41,
        0x41,
        0x40,
        0xde,
        0x5d,
        0xae,
        0x22,
        0x23,
        0xb0,
        0x03,
        0x61,
        0xa3,
        0x96,
        0x17,
        0x7a,
        0x9c,
        0xb4,
        0x10,
        0xff,
        0x61,
        0xf2,
        0x00,
        0x15,
        0xad,
    };
    unsigned char digest[SHA256_DIGEST_LENGTH];
    CHECK(SHA256(reinterpret_cast<const unsigned char *>("abc"), 3, digest));
    CHECK(!memcmp(digest, expected_sha, sizeof(digest)));
    /* Pinned BoringSSL aes_128_gcm_tests.txt: zero key, nonce and 16-byte plaintext. */
    const unsigned char expected_gcm[] = {
        0x03,
        0x88,
        0xda,
        0xce,
        0x60,
        0xb6,
        0xa3,
        0x92,
        0xf3,
        0x28,
        0xc2,
        0xb9,
        0x71,
        0xb2,
        0xfe,
        0x78,
        0xab,
        0x6e,
        0x47,
        0xd4,
        0x2c,
        0xec,
        0x13,
        0xbd,
        0xf5,
        0x3a,
        0x67,
        0xb2,
        0x12,
        0x57,
        0xbd,
        0xdf,
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
                                input, bytes, nullptr, 0) == 1 &&
              sealed_size == bytes + 16);
    }
    CHECK(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
    CHECK(EVP_AEAD_CTX_open(ctx, roundtrip, &opened_size, bytes, nonce, sizeof(nonce),
                            output, sealed_size, nullptr, 0) == 1);
    CHECK(opened_size == bytes && !memcmp(input, roundtrip, bytes));
    double elapsed = (finish.tv_sec - start.tv_sec) * 1000.0 + (finish.tv_nsec - start.tv_nsec) / 1e6;
    printf("{\"benchmark\":\"android_aes_gcm\",\"hwcap\":%lu,\"bytes\":%zu,\"elapsed_ms\":%.3f}\n",
           getauxval(AT_HWCAP), bytes * 64, elapsed);
    free(roundtrip);
    free(output);
    free(input);
    EVP_AEAD_CTX_free(ctx);
    printf("{\"test\":\"android_crypto\",\"passed\":true,\"fips\":%s,\"known_answers\":true,\"reject_bad_tag\":true}\n", fips ? "true" : "false");
}
