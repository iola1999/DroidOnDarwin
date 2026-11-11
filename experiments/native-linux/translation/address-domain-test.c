/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../layout.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static struct {
    uintptr_t address;
    size_t size;
    unsigned releases;
    int unmapped;
} regions[2];
static unsigned created, vmalloc_maps, vmalloc_revocations, ram_reclaims;

static int unmapped(uintptr_t address, size_t size)
{
    mach_vm_address_t start = address;
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
    return kr == KERN_INVALID_ADDRESS || start >= address + size;
}

/* Only the generated linux-execution.c test input uses these wrappers. */
kern_return_t nce_test_domain_map(vm_map_t task, mach_vm_address_t *address, mach_vm_size_t size,
                                  mach_vm_offset_t mask, int flags, mem_entry_name_port_t object,
                                  memory_object_offset_t offset, boolean_t copy, vm_prot_t protection,
                                  vm_prot_t maximum, vm_inherit_t inheritance)
{
    if (created >= 2 || task != mach_task_self() || *address != (created ? 1UL << 39 : 1UL << 36) || mask ||
        flags != VM_FLAGS_ANYWHERE || object != MEMORY_OBJECT_NULL || offset || copy ||
        maximum != VM_PROT_ALL || inheritance != VM_INHERIT_DEFAULT ||
        size != (created ? NCE_RAM_SIZE : NCE_RAM_SIZE + NCE_VMALLOC_SIZE) ||
        protection != (created ? VM_PROT_READ | VM_PROT_WRITE : VM_PROT_NONE))
        abort();
    /* Keep the original sparse host allocation hint; only guest bounds grow. */
    *address = 1UL << 36;
    kern_return_t result = mach_vm_map(task, address, size, mask, flags, object, offset, copy, protection, maximum, inheritance);
    if (result != KERN_SUCCESS)
        return result;
    if (*address < 0x10000 || *address > (1UL << 39) - size) {
        fprintf(stderr, "address-domain mapping outside guest range: index=%u address=%llx size=%llx\n",
                created, (unsigned long long)*address, (unsigned long long)size);
        abort();
    }
    regions[created].address = *address;
    regions[created].size = size;
    ++created;
    return KERN_SUCCESS;
}

kern_return_t nce_test_domain_remap(vm_map_t task, vm_address_t *address, vm_size_t size,
                                    vm_address_t mask, int flags, vm_map_t source_task, vm_address_t source,
                                    boolean_t copy, vm_prot_t *current, vm_prot_t *maximum, vm_inherit_t inheritance)
{
    kern_return_t result = vm_remap(task, address, size, mask, flags, source_task, source, copy, current, maximum, inheritance);
    if (result == KERN_SUCCESS && *address >= regions[0].address + NCE_RAM_SIZE &&
        *address < regions[0].address + regions[0].size)
        ++vmalloc_maps;
    return result;
}

void *nce_test_domain_mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset)
{
    void *result = mmap(address, size, protection, flags, fd, offset);
    if (result == MAP_FAILED || !(flags & MAP_FIXED))
        return result;
    uintptr_t start = (uintptr_t)address;
    if (protection == PROT_NONE && start >= regions[0].address + NCE_RAM_SIZE &&
        start < regions[0].address + regions[0].size)
        ++vmalloc_revocations;
    if (protection == (PROT_READ | PROT_WRITE) && start >= regions[1].address &&
        start < regions[1].address + regions[1].size)
        ++ram_reclaims;
    return result;
}

int nce_test_domain_unmap(void *address, size_t size)
{
    unsigned index;
    for (index = 0; index < created; ++index)
        if (regions[index].address == (uintptr_t)address && regions[index].size == size)
            break;
    if (index == created || regions[index].releases++)
        abort();
    const char *tail = getenv("NCE_TEST_DOMAIN_RETAIN_TAIL");
    size_t released = index == 0 && tail && !strcmp(tail, "1") ? size - (size_t)getpagesize() : size;
    int result = munmap(address, released);
    if (!result)
        regions[index].unmapped = unmapped((uintptr_t)address, size);
    return result;
}

__attribute__((destructor)) static void report_domains(void)
{
    int passed = created == 2;
    for (unsigned i = 0; i < created; ++i) {
        passed &= regions[i].releases == 1 && regions[i].unmapped;
        printf("{\"address_domain_region\":%u,\"address\":%llu,\"bytes\":%zu,\"releases\":%u,\"unmapped\":%s}\n",
               i, (unsigned long long)regions[i].address, regions[i].size, regions[i].releases,
               regions[i].unmapped ? "true" : "false");
    }
    printf("{\"test\":\"translated_host_guest_numeric_overlap\",\"passed\":%s,\"created\":%u,\"vmalloc_maps\":%u,\"vmalloc_revocations\":%u,\"ram_reclaims\":%u}\n",
           passed ? "true" : "false", created, vmalloc_maps, vmalloc_revocations, ram_reclaims);
}
