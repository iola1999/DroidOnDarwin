/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "code-watch.h"
#include <assert.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_BYTES 16384
static void *outside;
static volatile sig_atomic_t forwarded;
static _Atomic unsigned writers_ready, writers_go;

static void old_fault(int signal, siginfo_t *info, void *context)
{
    (void)signal;
    (void)context;
    if (info->si_addr != outside || mprotect(outside, PAGE_BYTES, PROT_READ | PROT_WRITE))
        _exit(125);
    ++forwarded;
}

static void alias_page(void *target, void *source, size_t size)
{
    vm_address_t address = (uintptr_t)target;
    vm_prot_t current, maximum;
    assert(vm_remap(mach_task_self(), &address, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                    mach_task_self(), (uintptr_t)source, FALSE, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(mprotect(target, size, PROT_READ | PROT_WRITE) == 0);
}

static void report(const char *name)
{
    printf("{\"test\":\"%s\",\"passed\":true}\n", name);
}

static void *writer(void *address)
{
    atomic_fetch_add_explicit(&writers_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&writers_go, memory_order_acquire))
        sched_yield();
    *(volatile unsigned *)address = 37;
    return NULL;
}

static void check(void *unused)
{
    (void)unused;
    const size_t size = 4 * PAGE_BYTES;
    unsigned char *ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    unsigned char *alias = mmap(NULL, size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(ram != MAP_FAILED && alias != MAP_FAILED);
    alias_page(alias, ram, size);
    outside = mmap(NULL, PAGE_BYTES, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(outside != MAP_FAILED);
    struct sigaction previous_bus, previous_segv, action = {.sa_sigaction = old_fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGBUS, &action, &previous_bus) && !sigaction(SIGSEGV, &action, &previous_segv));
    assert(!nce_code_watch_create(ram + 1, alias, size) && errno == EINVAL);
    assert(!nce_code_watch_create(ram, ram, size) && errno == EINVAL);
    NceCodeWatch *watch = nce_code_watch_create(ram, alias, size);
    assert(watch && !nce_code_watch_create(ram, alias, size) && errno == EBUSY);
    assert(!nce_code_watch_version(watch, ram + size));
    assert(nce_code_watch_invalidate(watch, 1, PAGE_BYTES) == -1 && errno == EINVAL);
    assert(nce_code_watch_invalidate(watch, size, PAGE_BYTES) == -1 && errno == EINVAL);
    report("code_watch_configuration_and_bounds");

    uint64_t first = nce_code_watch_version(watch, ram);
    assert(first && nce_code_watch_version(watch, ram + 4096) == first);
    errno = EDOM;
    *(volatile unsigned char *)ram = 17;
    assert(errno == EDOM && *(volatile unsigned char *)alias == 17);
    uint64_t second = nce_code_watch_version(watch, ram);
    assert(second > first);
    *(volatile unsigned char *)alias = 23;
    assert(*(volatile unsigned char *)ram == 23);
    uint64_t third = nce_code_watch_version(watch, ram);
    assert(third > second && nce_code_watch_version(watch, ram) == third);
    report("code_watch_both_aliases_and_reprotection");

    *(volatile unsigned char *)outside = 11;
    assert(forwarded == 1);
    assert(!mprotect(outside, PAGE_BYTES, PROT_NONE));
    assert(*(volatile unsigned char *)outside == 11 && forwarded == 2);
    report("code_watch_unrelated_read_and_write_forwarding");

    for (unsigned round = 0; round < 16; ++round) {
        uint64_t before = nce_code_watch_version(watch, ram + PAGE_BYTES);
        atomic_store(&writers_ready, 0);
        atomic_store(&writers_go, 0);
        pthread_t threads[4];
        for (unsigned i = 0; i < 4; ++i)
            assert(!pthread_create(&threads[i], NULL, writer, (i & 1 ? alias : ram) + PAGE_BYTES + i * 4));
        while (atomic_load_explicit(&writers_ready, memory_order_acquire) < 4)
            sched_yield();
        atomic_store_explicit(&writers_go, 1, memory_order_release);
        for (unsigned i = 0; i < 4; ++i) {
            assert(!pthread_join(threads[i], NULL));
            assert(*(unsigned *)(ram + PAGE_BYTES + i * 4) == 37);
        }
        assert(nce_code_watch_version(watch, ram + PAGE_BYTES) > before);
    }
    report("code_watch_concurrent_fault_retry");

    assert(!nce_code_watch_invalidate(watch, 0, PAGE_BYTES));
    assert(mmap(ram, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == ram);
    alias_page(alias, ram, PAGE_BYTES);
    assert(*(volatile unsigned char *)ram == 0);
    assert(nce_code_watch_version(watch, ram) > third);
    report("code_watch_backing_replacement");

    assert(vm_protect(mach_task_self(), (uintptr_t)alias + 2 * PAGE_BYTES, PAGE_BYTES, TRUE, VM_PROT_NONE) == KERN_SUCCESS);
    assert(!nce_code_watch_version(watch, ram + 2 * PAGE_BYTES));
    alias_page(alias + 2 * PAGE_BYTES, ram + 2 * PAGE_BYTES, PAGE_BYTES);
    assert(nce_code_watch_version(watch, ram + 2 * PAGE_BYTES));
    NceCodeWatchStats stats = nce_code_watch_stats(watch);
    assert(stats.queries > 0 && stats.write_faults >= 18 && stats.protections >= 20);
    assert(stats.invalidations == 1 && stats.protection_failures == 1);
    report("code_watch_protection_failure_and_retry");

    assert(vm_protect(mach_task_self(), (uintptr_t)ram + 3 * PAGE_BYTES, PAGE_BYTES, TRUE, VM_PROT_NONE) == KERN_SUCCESS);
    assert(!nce_code_watch_version(watch, ram + 3 * PAGE_BYTES));
    *(volatile unsigned char *)(alias + 3 * PAGE_BYTES) = 71;
    alias_page(ram + 3 * PAGE_BYTES, alias + 3 * PAGE_BYTES, PAGE_BYTES);
    assert(nce_code_watch_version(watch, ram + 3 * PAGE_BYTES));
    assert(*(volatile unsigned char *)(ram + 3 * PAGE_BYTES) == 71);
    report("code_watch_partial_protection_rollback");

    unsigned char *third_alias = mmap(NULL, PAGE_BYTES, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(third_alias != MAP_FAILED);
    assert(!nce_code_watch_exclude(watch, 3 * PAGE_BYTES, PAGE_BYTES));
    alias_page(third_alias, ram + 3 * PAGE_BYTES, PAGE_BYTES);
    *(volatile unsigned char *)third_alias = 83;
    assert(*(volatile unsigned char *)(ram + 3 * PAGE_BYTES) == 83);
    assert(!nce_code_watch_version(watch, ram + 3 * PAGE_BYTES));
    assert(!nce_code_watch_exclude(watch, 3 * PAGE_BYTES, PAGE_BYTES));
    assert(!nce_code_watch_invalidate(watch, 3 * PAGE_BYTES, PAGE_BYTES));
    assert(!nce_code_watch_version(watch, ram + 3 * PAGE_BYTES));
    assert(nce_code_watch_stats(watch).excluded_pages == 1);
    assert(!munmap(third_alias, PAGE_BYTES));
    report("code_watch_additional_alias_uses_content_checks");

    FILE *input = tmpfile();
    assert(input && fputc(97, input) == 97 && !fflush(input));
    first = nce_code_watch_version(watch, ram);
    assert(first && !nce_code_watch_invalidate(watch, 0, PAGE_BYTES));
    assert(pread(fileno(input), alias, 1, 0) == 1 && *ram == 97);
    assert(nce_code_watch_version(watch, ram) > first);
    assert(!fclose(input));
    report("code_watch_kernel_copyout_after_invalidation");
    nce_code_watch_destroy(watch);
    *(volatile unsigned char *)ram = 51;
    *(volatile unsigned char *)alias = 63;
    assert(*(volatile unsigned char *)ram == 63);
    struct sigaction restored;
    assert(!sigaction(SIGBUS, NULL, &restored) && restored.sa_sigaction == old_fault);
    assert(!sigaction(SIGSEGV, NULL, &restored) && restored.sa_sigaction == old_fault);
    assert(!sigaction(SIGBUS, &previous_bus, NULL) && !sigaction(SIGSEGV, &previous_segv, NULL));
    assert(!munmap(ram, size) && !munmap(alias, size) && !munmap(outside, PAGE_BYTES));
    report("code_watch_shutdown_restores_permissions_and_handlers");
    puts("{\"result\":\"passed\"}");
    exit(0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && (!strcmp(argv[1], "cli") || !strcmp(argv[1], "gcd")));
    setbuf(stdout, NULL);
    if (!strcmp(argv[1], "gcd")) {
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), NULL, check);
        dispatch_main();
    }
    check(NULL);
}
