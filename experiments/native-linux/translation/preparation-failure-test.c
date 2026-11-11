/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../code-memory.h"
#include "../layout.h"
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern NceCode real_prepare_range(void *, size_t);
extern int real_release_range(void *, size_t, int);
static struct {
    NceCode code;
    unsigned released;
} prepared[16];
static unsigned attempts, count;
static void *reservation;
static size_t reservation_size;
static unsigned reservation_releases;
static int reservation_unmapped;

/* Only linux-execution.c calls these wrappers in the failure executable. */
void *nce_test_reservation_mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset)
{
    const char *layout = getenv("NCE_TRANSLATION_GUEST_LAYOUT");
    size_t expected = NCE_RAM_SIZE + NCE_VMALLOC_SIZE;
    if (!layout || !strcmp(layout, "host"))
        expected += NCE_USER_SIZE;
    else if (strcmp(layout, "low"))
        abort();
    if (reservation || address || size != expected || protection != PROT_NONE || flags != (MAP_PRIVATE | MAP_ANON) || fd != -1 || offset)
        abort();
    void *result = mmap(address, size, protection, flags, fd, offset);
    if (result != MAP_FAILED) {
        reservation = result;
        reservation_size = size;
    }
    return result;
}

int nce_test_reservation_munmap(void *address, size_t size)
{
    if (!reservation || address != reservation || size != reservation_size || reservation_releases++)
        abort();
    const char *retain_tail = getenv("NCE_TEST_RESERVATION_RETAIN_TAIL");
    size_t released = retain_tail && !strcmp(retain_tail, "1") ? size - (size_t)getpagesize() : size;
    int result = munmap(address, released);
    if (!result) {
        mach_vm_address_t start = (uintptr_t)address;
        mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &start, &length, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
        if (MACH_PORT_VALID(object))
            mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS && kr != KERN_INVALID_ADDRESS)
            abort();
        reservation_unmapped = kr == KERN_INVALID_ADDRESS || start >= (uintptr_t)address + size;
    }
    return result;
}

/* Linked only into the failure regression executable. */
NceCode nce_code_prepare_range(void *address, size_t size)
{
    const char *selected = getenv("NCE_TEST_PREPARATION_FAIL_AT");
    if (++attempts == (unsigned)(selected ? atoi(selected) : 0)) {
        errno = EACCES;
        return (NceCode){0};
    }
    NceCode code = real_prepare_range(address, size);
    if (code.rw) {
        if (count == sizeof(prepared) / sizeof(*prepared))
            abort();
        prepared[count++].code = code;
    }
    return code;
}

int nce_code_release_range(void *address, size_t size, int is_prepared)
{
    int result = real_release_range(address, size, is_prepared);
    if (!result) {
        for (unsigned i = 0; i < count; ++i) {
            NceCode code = prepared[i].code;
            if (code.size != size)
                continue;
            if (code.rw == address)
                prepared[i].released |= 1;
            if (code.rx == address)
                prepared[i].released |= 2;
        }
    }
    return result;
}

__attribute__((destructor)) static void check_release(void)
{
    for (unsigned i = 0; i < count; ++i)
        if (prepared[i].released != 3)
            abort();
    printf("{\"test\":\"partial_preparation_cleanup\",\"passed\":true,\"attempts\":%u,\"prepared\":%u}\n", attempts, count);
    const char *layout = getenv("NCE_TRANSLATION_GUEST_LAYOUT");
    int passed = reservation ? reservation_releases == 1 && reservation_unmapped : reservation_releases == 0;
    printf("{\"test\":\"partial_reservation_cleanup\",\"passed\":%s,\"layout\":\"%s\",\"created\":%s,\"address\":%llu,\"bytes\":%zu,\"releases\":%u,\"unmapped\":%s}\n",
           passed ? "true" : "false", layout ? layout : "host", reservation ? "true" : "false",
           (unsigned long long)(uintptr_t)reservation, reservation_size, reservation_releases,
           reservation_unmapped ? "true" : "false");
}
