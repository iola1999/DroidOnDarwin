/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "code-memory.h"
#include <TargetConditionals.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include "ios-jit.h"
#endif

NceCode nce_code_prepare_range(void *address, size_t size)
{
    if (!address || !size || ((uintptr_t)address | size) % 16384 ||
        size > UINTPTR_MAX - (uintptr_t)address || getpagesize() != 16384) {
        errno = EINVAL;
        return (NceCode){0};
    }
    if (mprotect(address, size, PROT_READ | PROT_EXEC))
        return (NceCode){0};
#if TARGET_OS_IPHONE
    JITDualMapping mapping = dod_ios_jit_prepare_range(address, size);
    if (!mapping.rw_addr) {
        errno = EACCES;
        return (NceCode){0};
    }
    return (NceCode){mapping.rw_addr, mapping.rx_addr, mapping.size};
#else
    vm_address_t rw = 0;
    vm_prot_t current, maximum;
    kern_return_t result = vm_remap(mach_task_self(), &rw, size, 0, VM_FLAGS_ANYWHERE,
                                    mach_task_self(), (uintptr_t)address, FALSE,
                                    &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) {
        errno = result == KERN_PROTECTION_FAILURE ? EACCES : ENOMEM;
        return (NceCode){0};
    }
    if (mprotect((void *)rw, size, PROT_READ | PROT_WRITE)) {
        int error = errno;
        vm_deallocate(mach_task_self(), rw, size);
        errno = error;
        return (NceCode){0};
    }
    return (NceCode){(void *)rw, address, size};
#endif
}

int nce_code_release_range(void *address, size_t size, int prepared)
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
