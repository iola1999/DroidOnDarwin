/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "translation/read-view.h"
#include "translation/linux-memory.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_BYTES 16384UL
#define VIEW_BYTES (UINT64_C(1) << 32)
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "read-view:%d: %s errno=%d\n", __LINE__, #condition, errno); \
            exit(1); \
        } \
    } while (0)

static sigjmp_buf fault_return;
static volatile sig_atomic_t armed;
static volatile uintptr_t fault_address;
static void handler(int number, siginfo_t *info, void *context)
{
    (void)context;
    if (armed && (uintptr_t)info->si_addr == fault_address)
        siglongjmp(fault_return, 1);
    _exit(128 + number);
}
static void expect_fault(void *address, bool write)
{
    fault_address = (uintptr_t)address;
    armed = 1;
    if (!sigsetjmp(fault_return, 1)) {
        if (write)
            *(volatile uint64_t *)address = UINT64_MAX;
        else
            (void)*(volatile uint64_t *)address;
        CHECK(false);
    }
    armed = 0;
}
static uint64_t read_value(NceReadView *view, size_t offset)
{
    return *(volatile uint64_t *)((char *)view->base + offset);
}
static void report(const char *name)
{
    printf("{\"test\":\"%s\",\"passed\":true}\n", name);
}
static void probe(void *unused)
{
    (void)unused;
    translation_memory_phase("reserve");
    NceReadView a = {0}, b = {0};
    CHECK(!nce_read_view_create(&a, VIEW_BYTES));
    CHECK(!nce_read_view_create(&b, VIEW_BYTES));
    CHECK((uintptr_t)a.base + a.size <= (uintptr_t)b.base || (uintptr_t)b.base + b.size <= (uintptr_t)a.base);
    size_t ram_size = 16 * PAGE_BYTES;
    uint64_t *ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(ram != MAP_FAILED);
    for (size_t i = 0; i < ram_size / 8; ++i)
        ram[i] = i + 17;
    struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO}, old_bus, old_segv;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGBUS);
    sigaddset(&action.sa_mask, SIGSEGV);
    CHECK(!sigaction(SIGBUS, &action, &old_bus) && !sigaction(SIGSEGV, &action, &old_segv));
    translation_memory_phase("aliases");
    CHECK(!nce_read_view_map(&a, 0, ram, PAGE_BYTES));
    CHECK(!nce_read_view_map(&b, 0, (char *)ram + PAGE_BYTES, PAGE_BYTES));
    CHECK(!nce_read_view_map(&a, VIEW_BYTES - PAGE_BYTES, (char *)ram + 2 * PAGE_BYTES, PAGE_BYTES));
    CHECK(!nce_read_view_map(&b, VIEW_BYTES - PAGE_BYTES, (char *)ram + 2 * PAGE_BYTES, PAGE_BYTES));
    CHECK(read_value(&a, 0) == 17 && read_value(&b, 0) == PAGE_BYTES / 8 + 17);
    ram[0] = 37;
    CHECK(read_value(&a, 0) == 37 && read_value(&b, 0) == PAGE_BYTES / 8 + 17);
    ram[2 * PAGE_BYTES / 8] = 91;
    CHECK(read_value(&a, VIEW_BYTES - PAGE_BYTES) == 91 && read_value(&b, VIEW_BYTES - PAGE_BYTES) == 91);
    report("read_views_private_shared_and_last_page");

    uint64_t maps = a.maps + b.maps;
    for (unsigned i = 0; i < 100000; ++i) {
        CHECK(read_value(&a, 0) == 37);
        CHECK(read_value(&b, 0) == PAGE_BYTES / 8 + 17);
    }
    CHECK(a.maps + b.maps == maps && !a.revocations && !b.revocations);
    report("read_views_context_selection_keeps_mappings");

    CHECK(!nce_read_view_map(&a, 0, (char *)ram + PAGE_BYTES, PAGE_BYTES));
    CHECK(read_value(&a, 0) == read_value(&b, 0));
    CHECK(!nce_read_view_revoke(&a, 0, PAGE_BYTES));
    expect_fault(a.base, false);
    CHECK(read_value(&b, 0) == PAGE_BYTES / 8 + 17);
    CHECK(!nce_read_view_map(&a, 0, ram, PAGE_BYTES));
    expect_fault(a.base, true);
    CHECK(read_value(&a, 0) == 37 && ram[0] == 37);
    report("read_views_replace_revoke_and_write_protection");

    const size_t offset = 64 * 1024 * 1024;
    CHECK(!nce_read_view_map(&a, offset, ram, ram_size));
    for (size_t i = 0; i < ram_size / 8; ++i)
        CHECK(read_value(&a, offset + i * 8) == ram[i]);
    CHECK(!nce_read_view_revoke(&a, offset + PAGE_BYTES, 2 * PAGE_BYTES));
    expect_fault((char *)a.base + offset + PAGE_BYTES, false);
    expect_fault((char *)a.base + offset + 2 * PAGE_BYTES, false);
    CHECK(read_value(&a, offset) == ram[0] && read_value(&a, offset + 3 * PAGE_BYTES) == ram[3 * PAGE_BYTES / 8]);
    report("read_views_contiguous_range_and_partial_revoke");

    maps = a.maps;
    CHECK(nce_read_view_map(&a, 1, ram, PAGE_BYTES) == -1 && errno == EINVAL);
    CHECK(nce_read_view_map(&a, 0, (char *)ram + 1, PAGE_BYTES) == -1 && errno == EINVAL);
    CHECK(nce_read_view_map(&a, VIEW_BYTES - PAGE_BYTES, ram, 2 * PAGE_BYTES) == -1 && errno == EINVAL);
    CHECK(nce_read_view_revoke(&a, SIZE_MAX - PAGE_BYTES + 1, PAGE_BYTES) == -1 && errno == EINVAL);
    CHECK(nce_read_view_create(&a, VIEW_BYTES) == -1 && errno == EINVAL);
    CHECK(a.maps == maps && read_value(&a, 0) == 37);
    report("read_views_invalid_ranges_preserve_mappings");

    translation_memory_phase("cleanup");
    CHECK(!nce_read_view_destroy(&a));
    CHECK(!a.base && !a.size);
    CHECK(!nce_read_view_create(&a, VIEW_BYTES));
    expect_fault(a.base, false);
    CHECK(read_value(&b, 0) == PAGE_BYTES / 8 + 17);
    CHECK(!nce_read_view_destroy(&a) && !nce_read_view_destroy(&b));
    CHECK(!munmap(ram, ram_size));
    CHECK(!sigaction(SIGBUS, &old_bus, NULL) && !sigaction(SIGSEGV, &old_segv, NULL));
    report("read_views_destroy_recreate_and_cleanup");
}
int main(int argc, char **argv)
{
    CHECK(argc == 2 && (!strcmp(argv[1], "cli") || !strcmp(argv[1], "gcd")));
    setbuf(stdout, NULL);
    printf("{\"entry\":\"%s\",\"views\":2,\"bytes_per_view\":%llu,\"memory_interval_ms\":100}\n", argv[1], VIEW_BYTES);
    translation_memory_start(100);
    if (!strcmp(argv[1], "gcd"))
        dispatch_sync_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), NULL, probe);
    else
        probe(NULL);
    translation_memory_stop();
    puts("{\"result\":\"passed\"}");
    return 0;
}
