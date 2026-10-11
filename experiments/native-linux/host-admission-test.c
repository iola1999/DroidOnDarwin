/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lkl-host-ops.h"
#include "lkl-image.h"
#include "translation/kernel-api.h"
#include <assert.h>
#include "translation/kernel-image.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <sched.h>
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
static atomic_int ready, go;
static void *worker(void *unused)
{
    (void)unused;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go))
        sched_yield();
    long args[6] = {0};
    assert(invoke(178, args) > 0);
    return NULL;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    assert(argc == 2);
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
    for (int round = 0; round < 128; ++round) {
        pthread_t threads[32];
        atomic_store(&ready, 0);
        atomic_store(&go, 0);
        for (int i = 0; i < 32; ++i)
            assert(!pthread_create(&threads[i], NULL, worker, NULL));
        while (atomic_load(&ready) != 32)
            sched_yield();
        atomic_store(&go, 1);
        for (int i = 0; i < 32; ++i)
            assert(!pthread_join(threads[i], NULL));
        printf("round=%d passed\n", round);
    }
    long (*halt)(void) = (void *)(rx + LKL_SYS_HALT);
    void (*cleanup)(void) = (void *)(rx + LKL_CLEANUP);
    assert(!halt());
    cleanup();
    assert(!translation_kernel_release(&image));
    assert(!munmap((void *)base, 5UL << 30));
    puts("host admission passed");
}
