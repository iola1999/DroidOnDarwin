/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include <mach/mach.h>
#include <stdio.h>
#include <sys/mman.h>

int main(void)
{
    const size_t size = 65536;
    void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (rw == MAP_FAILED)
        return 1;
    vm_address_t rx = 0;
    vm_prot_t current, maximum;
    if (vm_remap(mach_task_self(), &rx, size, 0, VM_FLAGS_ANYWHERE,
                 mach_task_self(), (uintptr_t)rw, FALSE, &current, &maximum,
                 VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect((void *)rx, size, PROT_READ | PROT_EXEC) != 0) {
        perror("executable alias");
        return 1;
    }
    int result = nce_probe_run((NceCode){rw, (void *)rx, size});
    munmap(rw, size);
    munmap((void *)rx, size);
    return result;
}
