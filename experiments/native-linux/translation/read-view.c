/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "read-view.h"
#include <errno.h>
#include <mach/mach.h>
#include <sys/mman.h>
#define PAGE_BYTES 16384UL

static int invalid(void)
{
    errno = EINVAL;
    return -1;
}

static int range(NceReadView *view, size_t offset, size_t size)
{
    return view && view->base && size && !(offset % PAGE_BYTES) && !(size % PAGE_BYTES) &&
           offset < view->size && size <= view->size - offset;
}

int nce_read_view_create(NceReadView *view, size_t size)
{
    if (!view || view->base || !size || size % PAGE_BYTES)
        return invalid();
    void *base = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
        return -1;
    *view = (NceReadView){.base = base, .size = size};
    return 0;
}

int nce_read_view_map(NceReadView *view, size_t offset, const void *source, size_t size)
{
    if (!range(view, offset, size) || !source || (uintptr_t)source % PAGE_BYTES)
        return invalid();
    vm_address_t target = (uintptr_t)view->base + offset;
    vm_prot_t current, maximum;
    kern_return_t result = vm_remap(mach_task_self(), &target, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                    mach_task_self(), (uintptr_t)source, FALSE, &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) {
        errno = result == KERN_NO_SPACE || result == KERN_RESOURCE_SHORTAGE ? ENOMEM : EFAULT;
        return -1;
    }
    if (mprotect((void *)target, size, PROT_READ))
        return -1;
    ++view->maps;
    return 0;
}

int nce_read_view_revoke(NceReadView *view, size_t offset, size_t size)
{
    if (!range(view, offset, size))
        return invalid();
    void *address = (char *)view->base + offset;
    if (mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
        return -1;
    ++view->revocations;
    return 0;
}

int nce_read_view_destroy(NceReadView *view)
{
    if (!view)
        return invalid();
    if (view->base && munmap(view->base, view->size))
        return -1;
    *view = (NceReadView){0};
    return 0;
}
