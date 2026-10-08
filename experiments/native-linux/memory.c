/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "memory.h"
#include "execute-check.h"
#include "layout.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if TARGET_OS_IPHONE
#include "husk-ios-jit.h"
extern int nce_wait_debugger_detach(unsigned timeout_ms);
#endif

#define PAGE NCE_PAGE_SIZE
#define POOL_BYTES (208UL * 1024 * 1024)
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "memory failure line %d: %s\n", __LINE__, #c); abort(); } } while (0)
static NceCode pool;
static size_t used;

NceCode nce_memory_take(size_t size)
{
    size = (size + PAGE - 1) & ~(PAGE - 1);
    REQUIRE(pool.rw && size && used <= pool.size && size <= pool.size - used);
    NceCode result = {pool.rw + used, pool.rx + used, size};
    used += size;
    return result;
}

/* Executable addresses retain their original prepared mapping. Process
 * switches replace code bytes through RW aliases; only data uses remap. */
static int verify_memory(void)
{
    NceCode pages = nce_memory_take(2 * PAGE);
    uint32_t first[] = {0x52800540, 0xd65f03c0}; /* mov w0,42; ret */
    uint32_t second[] = {0x52800560, 0xd65f03c0}; /* mov w0,43; ret */
    for (unsigned i = 0; i < 8; ++i) {
        memcpy(pages.rw, i % 2 ? second : first, sizeof(first));
        nce_sync_code(pages);
        printf("{\"phase\":\"execute_original_jit_address\",\"iteration\":%u}\n", i);
        if (nce_check_execute(pages, 42 + (int)(i % 2))) return -1;
    }
    puts("{\"test\":\"prepared_fixed_address_code_updates\",\"passed\":true,\"updates\":8}");
    memcpy(pages.rw, first, sizeof(first));
    memcpy(pages.rw + PAGE, second, sizeof(second));
    vm_address_t address = 0;
    REQUIRE(vm_allocate(mach_task_self(), &address, PAGE, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    for (unsigned i = 0; i < 8; ++i) {
        REQUIRE(mprotect((void *)address, PAGE, PROT_NONE) == 0);
        vm_prot_t current, maximum;
        REQUIRE(vm_remap(mach_task_self(), &address, PAGE, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                         mach_task_self(), (uintptr_t)pages.rw + (i % 2) * PAGE,
                         FALSE, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
        REQUIRE(mprotect((void *)address, PAGE, PROT_READ) == 0);
        REQUIRE(*(const uint32_t *)address == (i % 2 ? second[0] : first[0]));
    }
    REQUIRE(vm_deallocate(mach_task_self(), address, PAGE) == KERN_SUCCESS);
    puts("{\"test\":\"data_fixed_remap_after_detach\",\"passed\":true,\"switches\":8}");
    return 0;
}

int nce_memory_prepare(void)
{
    REQUIRE(!pool.rw);
    printf("{\"phase\":\"prepare_jit_pool\",\"bytes\":%lu}\n", POOL_BYTES);
#if TARGET_OS_IPHONE
    HuskDualMapping prepared = husk_ios_jit_allocate(POOL_BYTES);
    if (!prepared.rw_addr) {
        puts("{\"result\":\"failed\",\"reason\":\"jit_allocation\"}");
        return -1;
    }
    pool = (NceCode){prepared.rw_addr, prepared.rx_addr, prepared.size};
    husk_ios_jit_detach();
    if (nce_wait_debugger_detach(5000) != 0) {
        nce_memory_release();
        return -1;
    }
#else
    void *rw = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(rw != MAP_FAILED);
    vm_address_t rx = 0;
    vm_prot_t current, maximum;
    REQUIRE(vm_remap(mach_task_self(), &rx, POOL_BYTES, 0, VM_FLAGS_ANYWHERE,
                     mach_task_self(), (uintptr_t)rw, FALSE, &current, &maximum,
                     VM_INHERIT_NONE) == KERN_SUCCESS);
    REQUIRE(mprotect((void *)rx, POOL_BYTES, PROT_READ | PROT_EXEC) == 0);
    pool = (NceCode){rw, (void *)rx, POOL_BYTES};
#endif
    puts("{\"phase\":\"check_fixed_code_and_data_remap\"}");
    if (verify_memory()) { nce_memory_release(); return -1; }
    return 0;
}

void nce_memory_release(void)
{
    REQUIRE(munmap(pool.rw, pool.size) == 0);
    REQUIRE(munmap(pool.rx, pool.size) == 0);
    pool = (NceCode){0};
    used = 0;
}
