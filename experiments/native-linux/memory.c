/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "memory.h"
#include "execute-check.h"
#include "layout.h"
#include <TargetConditionals.h>
#include <errno.h>
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
#define SPACE_BYTES (NCE_USER_SIZE + NCE_RAM_SIZE + NCE_VMALLOC_SIZE)
#define RESERVED_BYTES (NCE_CODE_POOL_SIZE + SPACE_BYTES)
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "memory failure line %d: %s\n", __LINE__, #c); abort(); } } while (0)
static unsigned char *reservation;
static NceCode pool, guest_code;
static size_t used;

NceCode nce_memory_take(size_t size)
{
    size = (size + PAGE - 1) & ~(PAGE - 1);
    REQUIRE(pool.rw && size && used <= pool.size && size <= pool.size - used);
    NceCode result = {pool.rw + used, pool.rx + used, size};
    used += size;
    return result;
}

NceCode nce_memory_guest_code(void) { return guest_code; }

int nce_memory_remap(void *target, void *source, size_t size, int protection)
{
    vm_address_t address = (uintptr_t)target;
    vm_prot_t current, maximum;
    kern_return_t kr = vm_remap(mach_task_self(), &address, size, 0,
                                VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                (uintptr_t)source, FALSE, &current, &maximum, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        printf("{\"result\":\"failed\",\"reason\":\"vm_remap\",\"kr\":%d,\"target\":\"%p\",\"source\":\"%p\",\"bytes\":%zu}\n",
               kr, target, source, size);
        fprintf(stderr, "vm_remap: %s\n", mach_error_string(kr));
        return -1;
    }
    if (mprotect(target, size, protection)) {
        printf("{\"result\":\"failed\",\"reason\":\"mprotect\",\"errno\":%d,\"target\":\"%p\",\"bytes\":%zu,\"protection\":%d}\n",
               errno, target, size, protection);
        return -1;
    }
    return 0;
}

/* Prepare only executable ranges. Data VM entries must remain replaceable. */
static NceCode prepare_code(unsigned char *rx, size_t size)
{
    if (mprotect(rx, size, PROT_READ | PROT_EXEC)) {
        printf("{\"result\":\"failed\",\"reason\":\"prepare_rx\",\"errno\":%d}\n", errno);
        return (NceCode){0};
    }
#if TARGET_OS_IPHONE
    HuskDualMapping code = husk_ios_jit_prepare_range(rx, size);
    return (NceCode){code.rw_addr, code.rx_addr, code.size};
#else
    vm_address_t rw = 0;
    vm_prot_t current, maximum;
    kern_return_t kr = vm_remap(mach_task_self(), &rw, size, 0, VM_FLAGS_ANYWHERE,
                                mach_task_self(), (uintptr_t)rx, FALSE, &current,
                                &maximum, VM_INHERIT_NONE);
    REQUIRE(kr == KERN_SUCCESS && mprotect((void *)rw, size, PROT_READ | PROT_WRITE) == 0);
    return (NceCode){(void *)rw, rx, size};
#endif
}

static int verify_memory(void)
{
    NceCode pages = nce_memory_take(2 * PAGE);
    uint32_t first[] = {0x52800540, 0xd65f03c0}; /* mov w0,42; ret */
    uint32_t second[] = {0x52800560, 0xd65f03c0}; /* mov w0,43; ret */
    NceCode ranges[] = {pages, guest_code};
    for (unsigned range = 0; range < 2; ++range) {
        for (unsigned i = 0; i < 8; ++i) {
            memcpy(ranges[range].rw, i % 2 ? second : first, sizeof(first));
            nce_sync_code(ranges[range]);
            printf("{\"phase\":\"execute_original_jit_address\",\"range\":%u,\"iteration\":%u}\n", range, i);
            if (nce_check_execute(ranges[range], 42 + (int)(i % 2))) return -1;
        }
    }
    puts("{\"test\":\"prepared_fixed_address_code_updates\",\"passed\":true,\"ranges\":2,\"updates_per_range\":8}");

    unsigned char *backing = mmap(NULL, NCE_RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (backing == MAP_FAILED) return -1;
    memcpy(backing, first, sizeof(first));
    memcpy(backing + PAGE, second, sizeof(second));
    memcpy(backing + NCE_RAM_SIZE - PAGE, second, sizeof(second));
    unsigned char *base = reservation + NCE_CODE_POOL_SIZE;
    int result = -1;
    /* Probe the actual data destinations on both sides of the prepared code. */
    void *targets[] = {base, base + NCE_EXEC_OFFSET + NCE_EXEC_SIZE};
    for (unsigned t = 0; t < 2; ++t) {
        for (unsigned i = 0; i < 8; ++i) {
            if (nce_memory_remap(targets[t], backing + (i % 2) * PAGE, PAGE, PROT_READ)) goto done;
            if (*(uint32_t *)targets[t] != (i % 2 ? second[0] : first[0])) goto done;
        }
    }
    puts("{\"test\":\"data_fixed_remap_after_detach\",\"passed\":true,\"targets\":2,\"switches_per_target\":8}");
    unsigned char *physical = base + NCE_USER_SIZE;
    puts("{\"phase\":\"check_linux_ram_remap\"}");
    if (nce_memory_remap(physical, backing, NCE_RAM_SIZE, PROT_READ | PROT_WRITE)) goto done;
    if (*(uint32_t *)physical != first[0] || *(uint32_t *)(physical + NCE_RAM_SIZE - PAGE) != second[0]) goto done;
    *(uint32_t *)physical = 123;
    if (*(uint32_t *)backing != 123) goto done;
    puts("{\"test\":\"linux_ram_remap\",\"passed\":true,\"bytes\":67108864}");
    result = 0;
done:
    REQUIRE(munmap(backing, NCE_RAM_SIZE) == 0);
    return result;
}

int nce_memory_prepare(void)
{
    REQUIRE(!reservation);
    printf("{\"phase\":\"prepare_jit_pool\",\"reserved_bytes\":%lu,\"prepared_code_bytes\":%lu}\n",
           RESERVED_BYTES, NCE_CODE_POOL_SIZE + NCE_EXEC_SIZE);
    reservation = mmap(NULL, RESERVED_BYTES, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (reservation == MAP_FAILED) {
        reservation = NULL;
        puts("{\"result\":\"failed\",\"reason\":\"address_reservation\"}");
        return -1;
    }
    pool = prepare_code(reservation, NCE_CODE_POOL_SIZE);
    if (pool.rw) guest_code = prepare_code(reservation + NCE_CODE_POOL_SIZE + NCE_EXEC_OFFSET, NCE_EXEC_SIZE);
#if TARGET_OS_IPHONE
    husk_ios_jit_detach();
    if (nce_wait_debugger_detach(5000) != 0) {
        nce_memory_release();
        return -1;
    }
#endif
    if (!pool.rw || !guest_code.rw) {
        puts("{\"result\":\"failed\",\"reason\":\"jit_preparation\"}");
        nce_memory_release();
        return -1;
    }
    puts("{\"phase\":\"check_fixed_code_and_data_remap\"}");
    if (verify_memory()) { nce_memory_release(); return -1; }
    return 0;
}

static int release_range(void *address, size_t size, int prepared)
{
    if (!address) return 0;
    kern_return_t kr = vm_deallocate(mach_task_self(), (uintptr_t)address, size);
    if (kr == KERN_SUCCESS) return 0;
#if TARGET_OS_IPHONE
    /* iOS can retain prepared VM entries until this single-run process exits. */
    if (prepared && kr == KERN_PROTECTION_FAILURE) {
        printf("{\"phase\":\"retained_jit_mapping\",\"bytes\":%zu}\n", size);
        return 0;
    }
#else
    (void)prepared;
#endif
    printf("{\"result\":\"failed\",\"reason\":\"vm_deallocate\",\"kr\":%d,\"bytes\":%zu}\n", kr, size);
    return -1;
}

int nce_memory_release(void)
{
    int result = 0;
    result |= release_range(pool.rw, pool.size, 1);
    result |= release_range(guest_code.rw, guest_code.size, 1);
    if (reservation) {
        size_t end = NCE_CODE_POOL_SIZE + NCE_EXEC_OFFSET + NCE_EXEC_SIZE;
        result |= release_range(reservation, NCE_CODE_POOL_SIZE, 1);
        result |= release_range(reservation + NCE_CODE_POOL_SIZE, NCE_EXEC_OFFSET, 0);
        result |= release_range(reservation + NCE_CODE_POOL_SIZE + NCE_EXEC_OFFSET, NCE_EXEC_SIZE, 1);
        result |= release_range(reservation + end, RESERVED_BYTES - end, 0);
    }
    reservation = NULL;
    pool = guest_code = (NceCode){0};
    used = 0;
    return result;
}
