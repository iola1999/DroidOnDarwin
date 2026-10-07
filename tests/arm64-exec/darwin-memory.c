/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone fastmem platform experiment. All fixed mappings stay inside a
 * reservation owned by this process; no guest image or application is opened. */
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <setjmp.h>
#include <signal.h>
#endif

#include "benchmarks.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d\n", __LINE__); exit(1); } } while (0)

static uint64_t ns(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static vm_prot_t protection(vm_address_t address)
{
    vm_address_t start = address;
    vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    CHECK(vm_region_recurse_64(mach_task_self(), &start, &size, &depth,
          (vm_region_recurse_info_t)&info, &count) == KERN_SUCCESS);
    CHECK(start <= address && address - start < size);
    return info.protection;
}

static void map_at(vm_address_t destination, vm_address_t source,
                   size_t size, bool copy, vm_prot_t permissions)
{
    vm_address_t actual = destination;
    vm_prot_t current, maximum;
    CHECK(vm_remap(mach_task_self(), &actual, size, 0,
          VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), source,
          copy, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    CHECK(actual == destination && (maximum & permissions) == permissions);
    CHECK(vm_protect(mach_task_self(), actual, size, false, permissions) == KERN_SUCCESS);
}

#if !TARGET_OS_IPHONE
static sigjmp_buf fault_return;
static volatile sig_atomic_t expecting_fault, fault_signal;

static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
    (void)info;
    (void)context;
    if (!expecting_fault) _Exit(128 + signal_number);
    fault_signal = signal_number;
    siglongjmp(fault_return, 1);
}

static void expect_fault(vm_address_t address, bool write)
{
    fault_signal = 0;
    expecting_fault = 1;
    if (sigsetjmp(fault_return, 1) == 0) {
        if (write) *(volatile uint64_t *)address = 99;
        else (void)*(volatile uint64_t *)address;
    }
    expecting_fault = 0;
    CHECK(fault_signal == SIGBUS || fault_signal == SIGSEGV);
}
#endif

int main(void)
{
    const size_t page = (size_t)getpagesize();
    CHECK(page >= 4096 && (page & (page - 1)) == 0);
    printf("host_page_bytes=%zu\n", page);

    /* Address space reservations do not touch their pages or allocate a guest
     * RAM copy. Release each one before trying the next size. */
    const unsigned bits[] = {32, 34, 36, 37, 39};
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
        size_t size = UINT64_C(1) << bits[i];
        void *reservation = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        printf("reserve_bits=%u success=%d\n", bits[i], reservation != MAP_FAILED);
        if (reservation != MAP_FAILED) CHECK(munmap(reservation, size) == 0);
    }

    void *ram = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    void *window = mmap(NULL, page * 4, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(ram != MAP_FAILED && window != MAP_FAILED);
    vm_address_t a = (vm_address_t)ram, b = a + page;
    vm_address_t first = (vm_address_t)window, second = first + page;
    vm_address_t snapshot = second + page, guard = snapshot + page;
    *(volatile uint64_t *)a = 11;
    *(volatile uint64_t *)b = 22;
    map_at(first, a, page, false, VM_PROT_READ | VM_PROT_WRITE);
    map_at(second, a, page, false, VM_PROT_READ);
    *(volatile uint64_t *)first = 33;
    CHECK(*(volatile uint64_t *)a == 33 && *(volatile uint64_t *)second == 33);
    CHECK(protection(first) == (VM_PROT_READ | VM_PROT_WRITE));
    CHECK(protection(second) == VM_PROT_READ && protection(guard) == VM_PROT_NONE);
    puts("PASS shared physical aliases and independent host-page permissions");

    map_at(snapshot, a, page, true, VM_PROT_READ | VM_PROT_WRITE);
    *(volatile uint64_t *)snapshot = 44;
    CHECK(*(volatile uint64_t *)a == 33 && *(volatile uint64_t *)first == 33);
    *(volatile uint64_t *)first = 55;
    CHECK(*(volatile uint64_t *)snapshot == 44 && *(volatile uint64_t *)second == 55);
    puts("PASS copy-on-write aliases");

    /* Check the effective protection granularity without accessing denied
     * memory. A 4 KiB subrange may protect its entire 16 KiB host page. */
    if (page > 4096) {
        CHECK(vm_protect(mach_task_self(), first + 4096, 4096, false, VM_PROT_READ) == KERN_SUCCESS);
        printf("subpage_protection first=%d requested=%d last=%d\n",
               protection(first), protection(first + 4096), protection(first + page - 1));
        CHECK(!(protection(first + 4096) & VM_PROT_WRITE));
        CHECK(vm_protect(mach_task_self(), first, page, false,
                        VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS);
    }

    uint64_t started = ns();
    const unsigned switches = 2000;
    for (unsigned i = 0; i < switches; ++i) {
        vm_address_t source = i & 1 ? a : b;
        map_at(first, source, page, false, VM_PROT_READ | VM_PROT_WRITE);
        CHECK(*(volatile uint64_t *)first == (i & 1 ? 55 : 22));
    }
    printf("remap_protect_ns_per_page=%.1f samples=%u\n", (ns() - started) / (double)switches, switches);
    CHECK(*(volatile uint64_t *)second == 55 && *(volatile uint64_t *)snapshot == 44);
    puts("PASS address-space replacement and retained independent aliases");

    *(volatile uint64_t *)first = 0;
    started = ns();
    memory_loop((volatile unsigned long *)first, MEMORY_ITERATIONS);
    printf("native_mapped_memory_ns=%llu iterations=%u\n",
           (unsigned long long)(ns() - started), MEMORY_ITERATIONS);
    CHECK(*(volatile uint64_t *)a == MEMORY_ITERATIONS);
    CHECK(*(volatile uint64_t *)second == MEMORY_ITERATIONS);

#if !TARGET_OS_IPHONE
    /* The standalone host test owns these handlers. Do not install them in
     * Husk: its debugger, crash logger and JIT have separate fault handling. */
    struct sigaction action = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
    struct sigaction old_bus, old_segv;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGBUS, &action, &old_bus) == 0);
    CHECK(sigaction(SIGSEGV, &action, &old_segv) == 0);
    expect_fault(second, true);
    expect_fault(guard, false);
    CHECK(vm_protect(mach_task_self(), first, page, false, VM_PROT_NONE) == KERN_SUCCESS);
    expect_fault(first, false);
    CHECK(*(volatile uint64_t *)a == MEMORY_ITERATIONS);
    CHECK(sigaction(SIGBUS, &old_bus, NULL) == 0);
    CHECK(sigaction(SIGSEGV, &old_segv, NULL) == 0);
    puts("PASS write denial, unmapped access and permission revocation faults");
#endif
    CHECK(munmap(window, page * 4) == 0);
    CHECK(munmap(ram, page * 2) == 0);
    puts("DARWIN-MEMORY PASS");
    return 0;
}
