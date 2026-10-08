/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "execute-check.h"
#include <assert.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void original_handler(int signal) { (void)signal; }

int main(void)
{
    size_t size = getpagesize();
    unsigned char *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(rw != MAP_FAILED);
    vm_address_t rx = 0;
    vm_prot_t current, maximum;
    assert(vm_remap(mach_task_self(), &rx, size, 0, VM_FLAGS_ANYWHERE,
                    mach_task_self(), (uintptr_t)rw, FALSE, &current, &maximum,
                    VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(mprotect((void *)rx, size, PROT_READ | PROT_EXEC) == 0);
    uint32_t words[] = {0x52800540, 0xd65f03c0}; /* mov w0,42; ret */
    memcpy(rw, words, sizeof(words));
    sys_dcache_flush(rw, size);
    sys_icache_invalidate((void *)rx, size);
    NceCode code = {rw, (void *)rx, size};
    const int signals[] = {SIGBUS, SIGSEGV, SIGILL};
    for (unsigned i = 0; i < 3; ++i) assert(signal(signals[i], original_handler) != SIG_ERR);
    assert(nce_check_execute(code, 42) == 0);
    assert(nce_check_execute(code, 43) == -1);
    memset(rw, 0, sizeof(words)); /* UDF faults inside the checked range. */
    sys_dcache_flush(rw, size);
    sys_icache_invalidate((void *)rx, size);
    assert(nce_check_execute(code, 42) == -1);
    assert(mprotect((void *)rx, size, PROT_NONE) == 0);
    assert(nce_check_execute(code, 42) == -1);
    assert(mprotect((void *)rx, size, PROT_READ | PROT_EXEC) == 0);
    memcpy(rw, words, sizeof(words));
    sys_dcache_flush(rw, size);
    sys_icache_invalidate((void *)rx, size);
    assert(nce_check_execute(code, 42) == 0);
    for (unsigned i = 0; i < 3; ++i) {
        struct sigaction action;
        assert(sigaction(signals[i], NULL, &action) == 0);
        assert(action.sa_handler == original_handler);
    }
    assert(munmap(rw, size) == 0 && munmap((void *)rx, size) == 0);
    puts("execute regression: values, instruction/access faults, recovery and handler restoration passed");
}
