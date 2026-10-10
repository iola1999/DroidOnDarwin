/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mmu-host.h"
#include "layout.h"
#include "lkl-host-ops.h"
#include <assert.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

extern struct lkl_host_operations nce_lkl_host;
static unsigned char *window, *ram;
static unsigned long *views;
static const size_t window_size = NCE_USER_SIZE + NCE_RAM_SIZE + NCE_VMALLOC_SIZE;

static void init_ram(unsigned long size)
{
    assert(size == NCE_RAM_SIZE);
    ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(ram != MAP_FAILED);
}

static void *map_ram(void *address, unsigned long offset, unsigned long size, enum lkl_prot protection)
{
    uintptr_t base = (uintptr_t)window, target = (uintptr_t)address;
    assert(offset <= NCE_RAM_SIZE && size <= NCE_RAM_SIZE - offset);
    assert(target >= base && target - base <= window_size && size <= window_size - (target - base));
    vm_address_t result = target;
    vm_prot_t current, maximum;
    assert(vm_remap(mach_task_self(), &result, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                    mach_task_self(), (uintptr_t)ram + offset, FALSE,
                    &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(result == target);
    int prot = (protection & LKL_PROT_READ ? PROT_READ : 0) |
               (protection & LKL_PROT_WRITE ? PROT_WRITE : 0) |
               (protection & LKL_PROT_EXEC ? PROT_EXEC : 0);
    assert(mprotect(address, size, prot) == 0);
    return address;
}

static int unmap_ram(void *address, unsigned long size)
{
    uintptr_t base = (uintptr_t)window, target = (uintptr_t)address;
    assert(target >= base && target - base <= window_size && size <= window_size - (target - base));
    return mprotect(address, size, PROT_NONE);
}

void nce_smp_mmu_setup(NceSmpSetLayout set_layout)
{
    assert(getpagesize() == NCE_PAGE_SIZE);
    window = mmap(NULL, window_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(window != MAP_FAILED);
    views = calloc(2 * NCE_USER_SIZE / NCE_PAGE_SIZE, sizeof(*views));
    assert(views);
    nce_lkl_host.shmem_init = init_ram;
    nce_lkl_host.shmem_mmap = map_ram;
    nce_lkl_host.munmap = unmap_ram;
    set_layout((uintptr_t)window, NCE_MM_DEMAND, views, 3, NULL, 1, 0);
}

void nce_smp_mmu_cleanup(void)
{
    assert(munmap(window, window_size) == 0);
    assert(munmap(ram, NCE_RAM_SIZE) == 0);
    free(views);
}
