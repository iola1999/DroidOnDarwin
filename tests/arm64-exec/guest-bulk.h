/* SPDX-License-Identifier: GPL-2.0-or-later */
extern void husk_neon_copy(void *destination, const void *source, u64 bytes);
extern void husk_neon_set(void *destination, u64 value, u64 bytes);
extern u64 husk_bulk_register_check(u64 destination, u64 source, u64 bytes, u64 operation);

static u64 bulk_request(u64 destination, u64 source, u64 bytes, u64 operation)
{
    register u64 x0 __asm__("x0") = destination;
    register u64 x1 __asm__("x1") = source;
    register u64 x2 __asm__("x2") = bytes;
    register u64 x3 __asm__("x3") = operation;
    register u64 x8 __asm__("x8");
    __asm__ volatile("hlt #0x484d" : "=r"(x8) : "r"(x0), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x8;
}

static void bulk_tests(void)
{
    const u64 size = 65536, iterations = 1024;
    s64 region = call(222, 0, size * 3, 3, 0x22, (u64)-1, 0);
    CHECK(region > 0);
    unsigned char *source = (void *)region, *destination = source + size;
    for (u64 i = 0; i < size; ++i) {
        source[i] = (i * 31 + i / 4096) & 255;
        destination[i] = 0xa5;
    }
    CHECK(bulk_request(0, 0, 0, 0) == !HUSK_TEST_BULK_ENABLED);
    CHECK(bulk_request((u64)destination, (u64)source, size + 1, 0) == 1);
    CHECK(bulk_request((u64)destination, (u64)source, size, 0x100000000UL) == 1);
    CHECK(husk_bulk_register_check((u64)destination, (u64)source, size, 0) == 0);
    // Compare with a 64-byte NEON loop, using the same guest and working set.
    u64 start = ns(), completed = 0;
    for (u64 i = 0; i < iterations; ++i) {
        if (bulk_request((u64)destination, (u64)source, size, 0))
            husk_neon_copy(destination, source, size);
        else ++completed;
    }
    u64 elapsed = ns() - start;
    for (u64 i = 0; i < size; ++i) CHECK(destination[i] == source[i]);
    if (HUSK_TEST_BULK_ENABLED) CHECK(completed > iterations / 2);
    else CHECK(completed == 0);
    print("BENCH bulk_copy_ns="); number(elapsed); print(" bytes="); number(size * iterations);
    print(" completed="); number(completed); print("\n");
    start = ns(); completed = 0;
    for (u64 i = 0; i < iterations; ++i) {
        if (bulk_request((u64)destination, i & 255, size, 2))
            husk_neon_set(destination, i & 255, size);
        else ++completed;
    }
    elapsed = ns() - start;
    for (u64 i = 0; i < size; ++i) CHECK(destination[i] == 255);
    if (HUSK_TEST_BULK_ENABLED) CHECK(completed > iterations / 2);
    print("BENCH bulk_set_ns="); number(elapsed); print(" bytes="); number(size * iterations);
    print(" completed="); number(completed); print("\n");
    if (HUSK_TEST_BULK_ENABLED) {
        CHECK(bulk_request((u64)source + 7, (u64)source, size - 7, 1) == 0);
        for (u64 i = 7; i < size; ++i) CHECK(source[i] == (((i - 7) * 31 + (i - 7) / 4096) & 255));
        // A late read or write failure must leave the complete destination intact.
        CHECK(call(226, (u64)destination + size - 4096, 4096, 1, 0, 0, 0) == 0);
        CHECK(bulk_request((u64)destination, 0, size, 2) == 1);
        for (u64 i = 0; i < size; ++i) CHECK(destination[i] == 255);
        CHECK(call(226, (u64)destination + size - 4096, 4096, 3, 0, 0, 0) == 0);
        CHECK(call(226, (u64)source + size - 4096, 4096, 0, 0, 0, 0) == 0);
        CHECK(bulk_request((u64)destination, (u64)source, size, 0) == 1);
        for (u64 i = 0; i < size; ++i) CHECK(destination[i] == 255);
        CHECK(call(226, (u64)source + size - 4096, 4096, 3, 0, 0, 0) == 0);
        s64 child = call(220, 17, 0, 0, 0, 0, 0);
        CHECK(child >= 0);
        if (!child) {
            CHECK(bulk_request((u64)destination, 0x19, size, 2) == 1);
            for (u64 i = 0; i < size; ++i) CHECK(destination[i] == 255);
            husk_neon_set(destination, 0x19, size);
            CHECK(bulk_request((u64)destination, 0x23, size, 2) == 0);
            call(93, destination[0] == 0x23 ? 73 : 1, 0, 0, 0, 0, 0);
            for (;;) {}
        }
        int status = 0;
        CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child && status == (73 << 8));
        for (u64 i = 0; i < size; ++i) CHECK(destination[i] == 255);
    }
    CHECK(call(215, region, size * 3, 0, 0, 0, 0) == 0);
    print("PASS bulk bounds, register state, overlap, permissions and COW\n");
}
