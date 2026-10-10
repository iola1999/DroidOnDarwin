/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define PAGE 16384UL
#define SPAN (3UL * 1024 * 1024 * 1024)
#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "view-swap:%d: %s\n", __LINE__, #c); \
            exit(1); \
        } \
    } while (0)

static unsigned char *reserve(size_t bytes, int protection)
{
    void *result = mmap(NULL, bytes, protection, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(result != MAP_FAILED);
    return result;
}

static void alias(void *target, void *source, size_t bytes)
{
    mach_vm_address_t address = (uintptr_t)target;
    vm_prot_t current, maximum;
    kern_return_t result = mach_vm_remap(mach_task_self(), &address, bytes, 0,
                                         VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                         mach_task_self(), (uintptr_t)source, FALSE,
                                         &current, &maximum, VM_INHERIT_NONE);
    if (result)
        fprintf(stderr, "mach_vm_remap: %s\n", mach_error_string(result));
    CHECK(result == KERN_SUCCESS && address == (uintptr_t)target);
}

static int protection(void *pointer)
{
    mach_vm_address_t address = (uintptr_t)pointer;
    mach_vm_size_t bytes;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    CHECK(mach_vm_region(mach_task_self(), &address, &bytes, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS);
    if (object != MACH_PORT_NULL)
        CHECK(mach_port_deallocate(mach_task_self(), object) == KERN_SUCCESS);
    CHECK(address <= (uintptr_t)pointer && bytes > (uintptr_t)pointer - address);
    return info.protection;
}

static size_t offset(unsigned page, unsigned pages)
{
    return page == pages - 1 ? SPAN - PAGE : page * 32UL * PAGE;
}

static void benchmark(unsigned pages)
{
    unsigned char *ram = reserve((2 * pages + 1) * PAGE, PROT_READ | PROT_WRITE);
    unsigned char *views[2] = {reserve(SPAN, PROT_NONE), reserve(SPAN, PROT_NONE)};
    unsigned char *target = reserve(SPAN, PROT_NONE);
    for (unsigned side = 0; side < 2; ++side) {
        for (unsigned page = 0; page < pages; ++page) {
            size_t physical = (page == pages - 1 ? 2 * pages : side * pages + page) * PAGE;
            ram[physical] = page == pages - 1 ? 77 : side + 42;
            alias(views[side] + offset(page, pages), ram + physical, PAGE);
            if (page % 2)
                CHECK(mprotect(views[side] + offset(page, pages), PAGE, PROT_READ) == 0);
        }
    }
    ram[2 * pages * PAGE] = 78;
    CHECK(views[0][SPAN - PAGE] == 78 && views[1][SPAN - PAGE] == 78);
    ram[2 * pages * PAGE] = 77;
    for (unsigned turn = 0; turn < 8; ++turn) {
        unsigned side = turn % 2;
        alias(target, views[side], SPAN);
        CHECK(protection(target + PAGE) == PROT_NONE);
        for (unsigned page = 0; page < pages; ++page) {
            CHECK(target[offset(page, pages)] == (page == pages - 1 ? 77 : side + 42));
            CHECK(protection(target + offset(page, pages)) == (page % 2 ? PROT_READ : PROT_READ | PROT_WRITE));
        }
        target[0] = 99;
        CHECK(views[side][0] == 99 && views[1 - side][0] == 43 - side);
        target[0] = side + 42;
    }
    /* Replace a private backing page and its permissions before the next swap. */
    alias(views[1], ram, PAGE);
    CHECK(mprotect(views[1], PAGE, PROT_READ) == 0);
    alias(target, views[1], SPAN);
    CHECK(target[0] == 42 && protection(target) == PROT_READ);
    CHECK(mmap(views[1] + offset(2, pages), PAGE, PROT_NONE,
               MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) == views[1] + offset(2, pages));
    alias(target, views[1], SPAN);
    CHECK(protection(target + offset(2, pages)) == PROT_NONE);
    const unsigned rounds = 128;
    for (unsigned bulk = 0; bulk < 2; ++bulk) {
        uint64_t begin = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        for (unsigned turn = 0; turn < rounds; ++turn) {
            unsigned char *source = views[turn % 2];
            if (bulk)
                alias(target, source, SPAN);
            else
                for (unsigned page = 0; page < pages; ++page)
                    alias(target + offset(page, pages), source + offset(page, pages), PAGE);
        }
        uint64_t elapsed = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - begin;
        printf("{\"benchmark\":\"darwin_view_swap\",\"bulk\":%s,\"span\":%lu,\"mapped_pages\":%u,\"rounds\":%u,\"ns_per_switch\":%llu}\n",
               bulk ? "true" : "false", SPAN, pages, rounds, (unsigned long long)(elapsed / rounds));
    }
    CHECK(munmap(target, SPAN) == 0 && munmap(views[0], SPAN) == 0 && munmap(views[1], SPAN) == 0);
    CHECK(munmap(ram, (2 * pages + 1) * PAGE) == 0);
}

int main(void)
{
    for (unsigned pages = 16; pages <= 1024; pages *= 8)
        benchmark(pages);
    puts("{\"test\":\"darwin_sparse_view_swap\",\"passed\":true}");
    return 0;
}
