/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "reclaim-test:%d errno=%d\n", __LINE__, errno); _exit(113); } } while (0)
int main(void)
{
    const size_t size = 64 * 1024 * 1024, page = 16384;
    volatile uint64_t *retained = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(retained != MAP_FAILED);
    *retained = UINT64_C(0x18463527a0b9c8d7);
    for (unsigned round = 0; round < 2; ++round) {
        unsigned char *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        CHECK(data != MAP_FAILED);
        for (size_t offset = 0; offset < size; offset += page) {
            CHECK(data[offset] == 0);
            data[offset] = 0x60 + round;
        }
        for (size_t offset = 0; offset < size; offset += page) CHECK(data[offset] == 0x60 + round);
        CHECK(munmap(data, size) == 0);
        struct timespec delay = {3, 0};
        CHECK(nanosleep(&delay, NULL) == 0);
        CHECK(*retained == UINT64_C(0x18463527a0b9c8d7));
    }
    CHECK(munmap((void *)retained, page) == 0);
    puts("{\"test\":\"linux_reclaimed_ram_reuse\",\"passed\":true,\"bytes_per_round\":67108864,\"rounds\":2}");
    return 0;
}
