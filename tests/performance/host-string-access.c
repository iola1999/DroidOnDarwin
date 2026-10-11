/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lkl-host-ops.h"
#include "lkl-image.h"
#include "layout.h"
#include "translation/kernel-api.h"
#include <assert.h>
#include "translation/kernel-image.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <string.h>
#include <stdbool.h>
extern struct lkl_host_operations nce_lkl_host;
static unsigned char *ram;
static void shmem_init(unsigned long size)
{
    ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(ram != MAP_FAILED);
}
static void *shmem_map(void *addr, unsigned long offset, unsigned long size, enum lkl_prot prot)
{
    vm_address_t target = (uintptr_t)addr;
    vm_prot_t current, maximum;
    assert(vm_remap(mach_task_self(), &target, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (uintptr_t)ram + offset, FALSE, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(!mprotect((void *)target, size, prot));
    return (void *)target;
}
static int clear_map(void *addr, unsigned long size)
{
    return mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == addr ? 0 : -1;
}
static long (*invoke)(long, long *);
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    assert(argc == 3);
    bool expect_failure = !strcmp(argv[2], "failed");
    TranslationKernelImage image = {0};
    assert(!translation_kernel_load(&image, argv[1]));
    uintptr_t rx = (uintptr_t)image.base;
    int (*init)(struct lkl_host_operations *) = (void *)(rx + LKL_INIT);
    int (*start)(unsigned long, unsigned long *) = (void *)(rx + NCE_LKL_START);
    invoke = (void *)(rx + LKL_SYSCALL);
    unsigned long base = (unsigned long)mmap(NULL, 5UL << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert((void *)base != MAP_FAILED);
    struct nce_translation_state state = {.cpu_count = 1};
    void (*layout)(unsigned long, unsigned long, unsigned long *, unsigned long, void *, unsigned long, unsigned long) = (void *)(rx + NCE_SET_LAYOUT);
    layout(base, NCE_MM_TRANSLATED, (void *)&state, 3, NULL, 1, 0);
    nce_lkl_host.munmap = clear_map;
    nce_lkl_host.shmem_init = shmem_init;
    nce_lkl_host.shmem_mmap = shmem_map;
    printf("kernel_base=%p\n", (void *)rx);
    puts("calling init");
    assert(init(&nce_lkl_host) == 0);
    puts("calling start");
    unsigned long slice = 0;
    assert(start(0, &slice) == 0);
    char *high = mmap((void *)(base + (6UL << 30)), 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(high != MAP_FAILED && (uintptr_t)high > base + NCE_USER_SIZE);
    strcpy(high, "/proc");
    strcpy(high + 64, "proc");
    long mkdir_args[6] = {-100, (long)"/proc", 0755};
    assert(invoke(34, mkdir_args) == 0);
    long mount_args[6] = {(long)"proc", (long)"/proc", (long)(high + 64), 0, 0};
    long mounted = invoke(40, mount_args);
    assert(mounted == (expect_failure ? -14 : 0));
    long open_args[6] = {-100, (long)high, 0x84000, 0};
    long fd = invoke(56, open_args);
    assert(expect_failure ? fd == -14 : fd >= 0);
    if (!expect_failure) {
        long read_args[6] = {fd, (long)(high + 4096), 4096};
        assert(invoke(61, read_args) > 0);
        assert(invoke(57, read_args) == 0);
        strcpy(high, "/proc/self/status");
        open_args[2] = 0x80000;
        fd = invoke(56, open_args);
        assert(fd >= 0);
        read_args[0] = fd;
        assert(invoke(63, read_args) > 0 && invoke(57, read_args) == 0);
    }
    printf("{\"test\":\"host_string_above_guest_window\",\"expected_failure\":%s,\"mount\":%ld,\"open\":%ld,\"passed\":true}\n", expect_failure ? "true" : "false", mounted, fd);
    assert(!munmap(high, 16384));
    long (*halt)(void) = (void *)(rx + LKL_SYS_HALT);
    void (*cleanup)(void) = (void *)(rx + LKL_CLEANUP);
    assert(!halt());
    cleanup();
    assert(!translation_kernel_release(&image));
    assert(!munmap((void *)base, 5UL << 30));
    puts("host string access passed");
}
