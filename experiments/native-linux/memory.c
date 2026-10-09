/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "memory.h"
#include "execute-check.h"
#include "layout.h"
#include "programs.h"
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
#define REQUIRE(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "memory failure line %d: %s\n", __LINE__, #c); \
            abort(); \
        } \
    } while (0)
static unsigned char *reservation;
static NceCode pool, dynamic_code;
static size_t used;
static int code_protection;

NceCode nce_memory_take(size_t size)
{
    size = (size + PAGE - 1) & ~(PAGE - 1);
    REQUIRE(pool.rw && size && used <= pool.size && size <= pool.size - used);
    NceCode result = {pool.rw + used, pool.rx + used, size};
    used += size;
    return result;
}

uintptr_t nce_memory_user_base(void)
{
    return (uintptr_t)reservation + NCE_CODE_POOL_SIZE;
}
NceCode nce_memory_dynamic_code(void)
{
    return dynamic_code;
}
int nce_memory_code_protection(void)
{
    return code_protection;
}

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
    unsigned range = 0;
    for (size_t program = 0; program <= nce_program_count + 1; ++program) {
        size_t count = program && program <= nce_program_count ? nce_programs[program - 1].segment_count : 1;
        for (size_t segment = 0; segment < count; ++segment, ++range) {
            NceCode target = !program ? pages : program > nce_program_count ? dynamic_code
                                                                            : nce_programs[program - 1].segments[segment].code;
            for (unsigned i = 0; i < 8; ++i) {
                memcpy(target.rw, i % 2 ? second : first, sizeof(first));
                nce_sync_code((NceCode){target.rw, target.rx, sizeof(first)});
                printf("{\"phase\":\"execute_original_jit_address\",\"range\":%u,\"iteration\":%u}\n", range, i);
                if (nce_check_execute(target, 42 + (int)(i % 2)))
                    return -1;
            }
        }
    }
    printf("{\"test\":\"prepared_fixed_address_code_updates\",\"passed\":true,\"ranges\":%u,\"updates_per_range\":8}\n", range);

    /* Probe a private pool page: a rejected restoration cannot damage a guest
     * program's prepared range. Copy-based revocation remains available. */
    NceCode protection_probe = {pages.rw + PAGE, pages.rx + PAGE, PAGE};
    code_protection = 1;
    for (unsigned i = 0; i < 8; ++i) {
        if (mprotect(protection_probe.rx, PAGE, PROT_NONE) ||
            mprotect(protection_probe.rx, PAGE, PROT_READ | PROT_EXEC)) {
            code_protection = 0;
            break;
        }
        memcpy(protection_probe.rw, i % 2 ? second : first, sizeof(first));
        nce_sync_code((NceCode){protection_probe.rw, protection_probe.rx, sizeof(first)});
        if (nce_check_execute(protection_probe, 42 + (int)(i % 2)))
            return -1;
    }
    const char *force_copy = getenv("NCE_CODE_COPY");
    if (force_copy && !strcmp(force_copy, "1"))
        code_protection = 0;
    printf("{\"test\":\"prepared_code_permission_restore\",\"passed\":true,\"available\":%s}\n",
           code_protection ? "true" : "false");

    unsigned char *backing = mmap(NULL, NCE_RAM_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (backing == MAP_FAILED)
        return -1;
    memcpy(backing, first, sizeof(first));
    memcpy(backing + PAGE, second, sizeof(second));
    memcpy(backing + NCE_RAM_SIZE - PAGE, second, sizeof(second));
    unsigned char *base = reservation + NCE_CODE_POOL_SIZE;
    int result = -1;
    /* Probe the actual data destinations on both sides of the prepared code. */
    NceSegment *first_code = &nce_programs[0].segments[0];
    void *targets[] = {base, first_code->code.rx + first_code->size};
    for (unsigned t = 0; t < 2; ++t) {
        for (unsigned i = 0; i < 8; ++i) {
            if (nce_memory_remap(targets[t], backing + (i % 2) * PAGE, PAGE, PROT_READ))
                goto done;
            if (*(uint32_t *)targets[t] != (i % 2 ? second[0] : first[0]))
                goto done;
        }
    }
    puts("{\"test\":\"data_fixed_remap_after_detach\",\"passed\":true,\"targets\":2,\"switches_per_target\":8}");
    unsigned char *physical = base + NCE_USER_SIZE;
    puts("{\"phase\":\"check_linux_ram_remap\"}");
    if (nce_memory_remap(physical, backing, NCE_RAM_SIZE, PROT_READ | PROT_WRITE))
        goto done;
    if (*(uint32_t *)physical != first[0] || *(uint32_t *)(physical + NCE_RAM_SIZE - PAGE) != second[0])
        goto done;
    *(uint32_t *)physical = 123;
    if (*(uint32_t *)backing != 123)
        goto done;
    printf("{\"test\":\"linux_ram_remap\",\"passed\":true,\"bytes\":%lu}\n", NCE_RAM_SIZE);
    result = 0;
done:
    REQUIRE(munmap(backing, NCE_RAM_SIZE) == 0);
    return result;
}

int nce_memory_prepare(void)
{
    REQUIRE(!reservation);
    size_t prepared = NCE_CODE_POOL_SIZE + NCE_EXEC_SIZE;
    for (size_t i = 0; i < nce_program_count; ++i) {
        prepared += nce_programs[i].stub_size;
        for (size_t j = 0; j < nce_programs[i].segment_count; ++j)
            prepared += nce_programs[i].segments[j].size;
    }
    printf("{\"phase\":\"prepare_jit_pool\",\"reserved_bytes\":%lu,\"prepared_code_bytes\":%zu}\n",
           RESERVED_BYTES, prepared);
    const size_t alignment = 2 * 1024 * 1024;
    void *raw = mmap(NULL, RESERVED_BYTES + alignment, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (raw != MAP_FAILED && ((uintptr_t)raw >= NCE_HOST_ADDRESS_LIMIT ||
                              RESERVED_BYTES + alignment > NCE_HOST_ADDRESS_LIMIT - (uintptr_t)raw)) {
        REQUIRE(munmap(raw, RESERVED_BYTES + alignment) == 0);
        raw = MAP_FAILED;
        errno = ERANGE;
    }
    if (raw == MAP_FAILED) {
        int error = errno;
        reservation = NULL;
        printf("{\"result\":\"failed\",\"reason\":\"address_reservation\",\"errno\":%d,\"requested_bytes\":%zu}\n",
               error, RESERVED_BYTES + alignment);
#if TARGET_OS_IPHONE
        husk_ios_jit_detach();
        nce_wait_debugger_detach(5000);
#endif
        return -1;
    }
    reservation = (void *)(((uintptr_t)raw + alignment - 1) & ~(alignment - 1));
    size_t prefix = reservation - (unsigned char *)raw;
    if (prefix)
        REQUIRE(munmap(raw, prefix) == 0);
    REQUIRE(munmap(reservation + RESERVED_BYTES, alignment - prefix) == 0);
    pool = prepare_code(reservation, NCE_CODE_POOL_SIZE);
    dynamic_code = prepare_code((void *)(nce_memory_user_base() + NCE_EXEC_OFFSET), NCE_EXEC_SIZE);
    int valid = pool.rw != NULL && dynamic_code.rw != NULL;
    for (size_t i = 0; valid && i < nce_program_count; ++i) {
        NceProgram *p = &nce_programs[i];
        p->base = nce_memory_user_base() + p->offset;
        p->stubs = prepare_code((void *)(p->base + p->span), p->stub_size);
        valid = p->stubs.rw != NULL;
        for (size_t j = 0; valid && j < p->segment_count; ++j) {
            NceSegment *s = &p->segments[j];
            s->code = prepare_code((void *)(p->base + s->offset), s->size);
            valid = s->code.rw != NULL;
        }
    }
#if TARGET_OS_IPHONE
    husk_ios_jit_detach();
    if (nce_wait_debugger_detach(5000) != 0) {
        nce_memory_release();
        return -1;
    }
#endif
    if (!valid) {
        puts("{\"result\":\"failed\",\"reason\":\"jit_preparation\"}");
        nce_memory_release();
        return -1;
    }
    puts("{\"phase\":\"check_fixed_code_and_data_remap\"}");
    if (verify_memory()) {
        nce_memory_release();
        return -1;
    }
    return 0;
}

static int release_range(void *address, size_t size, int prepared)
{
    if (!address)
        return 0;
    kern_return_t kr = vm_deallocate(mach_task_self(), (uintptr_t)address, size);
    if (kr == KERN_SUCCESS)
        return 0;
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
    if (reservation) {
        result |= release_range(reservation, NCE_CODE_POOL_SIZE, 1);
        uintptr_t cursor = nce_memory_user_base();
        for (size_t i = 0; i < nce_program_count; ++i) {
            NceProgram *p = &nce_programs[i];
            for (size_t j = 0; j < p->segment_count; ++j) {
                NceSegment *s = &p->segments[j];
                uintptr_t begin = nce_memory_user_base() + p->offset + s->offset;
                REQUIRE(begin >= cursor);
                if (begin > cursor)
                    result |= release_range((void *)cursor, begin - cursor, 0);
                result |= release_range(s->code.rw, s->size, 1);
                if (s->source_cache)
                    REQUIRE(munmap(s->source_cache, s->size) == 0);
                free(s->cache_prot);
                s->source_cache = s->cache_prot = NULL;
                result |= release_range((void *)begin, s->size, s->code.rx != NULL);
                cursor = begin + s->size;
                s->code = (NceCode){0};
            }
            uintptr_t stub_start = nce_memory_user_base() + p->offset + p->span;
            REQUIRE(cursor <= stub_start);
            if (cursor < stub_start)
                result |= release_range((void *)cursor, stub_start - cursor, 0);
            result |= release_range(p->stubs.rw, p->stub_size, 1);
            result |= release_range((void *)stub_start, p->stub_size, p->stubs.rx != NULL);
            cursor = stub_start + p->stub_size;
            p->stubs = (NceCode){0};
        }
        uintptr_t dynamic_start = nce_memory_user_base() + NCE_EXEC_OFFSET;
        REQUIRE(cursor <= dynamic_start);
        if (cursor < dynamic_start)
            result |= release_range((void *)cursor, dynamic_start - cursor, 0);
        result |= release_range(dynamic_code.rw, dynamic_code.size, 1);
        result |= release_range((void *)dynamic_start, NCE_EXEC_SIZE, dynamic_code.rx != NULL);
        cursor = dynamic_start + NCE_EXEC_SIZE;
        result |= release_range((void *)cursor, (uintptr_t)reservation + RESERVED_BYTES - cursor, 0);
    }
    reservation = NULL;
    pool = (NceCode){0};
    dynamic_code = (NceCode){0};
    used = 0;
    return result;
}
