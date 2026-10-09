/* SPDX-License-Identifier: Apache-2.0 */
#include "reference_heap.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static size_t page_size;
static void *map_pages(void *address, size_t count, int extra_flags)
{
    return nce_art_heap_map(address, count * page_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | extra_flags, -1, 0);
}

static void *worker(void *argument)
{
    unsigned char pattern = (uintptr_t)argument;
    for (unsigned round = 0; round < 64; ++round) {
        unsigned char *area = map_pages(NULL, 3, 0);
        assert(area != MAP_FAILED);
        memset(area, pattern, 3 * page_size);
        for (size_t i = 0; i < 3 * page_size; ++i)
            assert(area[i] == pattern);
        assert(nce_art_decode_reference(nce_art_encode_reference(area)) == area);
        assert(nce_art_heap_unmap(area, 3 * page_size) == 0);
    }
    return NULL;
}

int main(void)
{
    assert(nce_art_heap_init() == 0);
    uintptr_t base = nce_art_reference_base;
    assert(base > UINT32_MAX && nce_art_heap_init() == 0 && base == nce_art_reference_base);
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    assert(nce_art_decode_reference(0) == NULL && nce_art_encode_reference(NULL) == 0);
    assert(nce_art_decode_reference(UINT32_MAX) == (void *)(base + UINT32_MAX));
    assert(nce_art_encode_reference((void *)(base + UINT32_MAX)) == UINT32_MAX);
    const uintptr_t invalid[] = {base - 1, base, base + (UINT64_C(1) << 32)};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            struct rlimit limit = {0, 0};
            assert(setrlimit(RLIMIT_CORE, &limit) == 0);
            nce_art_encode_reference((void *)invalid[i]);
            _exit(99);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
    assert(!nce_art_heap_contains((void *)(base - 1), 1));
    assert(!nce_art_heap_contains((void *)(base + UINT32_MAX), 2));
    assert(map_pages((void *)base, 1, 0) == MAP_FAILED);
    assert(map_pages(NULL, 1, MAP_FIXED) == MAP_FAILED);
    unsigned char *region = map_pages(NULL, 3, 0);
    assert(region != MAP_FAILED);
    memset(region, 0xa5, 3 * page_size);
    assert(map_pages(region, 1, MAP_FIXED_NOREPLACE) == MAP_FAILED && errno == EEXIST);
    assert(nce_art_heap_unmap(region + page_size, page_size) == 0);
    unsigned char *hole = map_pages(region + page_size, 1, MAP_FIXED_NOREPLACE);
    assert(hole == region + page_size);
    for (size_t i = 0; i < page_size; ++i) {
        assert(hole[i] == 0 && region[i] == 0xa5 && region[2 * page_size + i] == 0xa5);
    }
    assert(nce_art_heap_contains((void *)base, NCE_ART_HEAP_SIZE));
    assert(!nce_art_heap_contains((void *)(base + NCE_ART_HEAP_SIZE), 1));
    void *last = (void *)(base + NCE_ART_HEAP_SIZE - page_size);
    assert(map_pages(last, 1, MAP_FIXED_NOREPLACE) == last);
    *(unsigned char *)last = 0x53;
    assert(map_pages(last, 2, 0) == MAP_FAILED);
    assert(nce_art_heap_unmap(last, page_size) == 0);
    assert(nce_art_heap_unmap(region, 3 * page_size) == 0);
    assert(nce_art_heap_map(region, page_size, PROT_READ, MAP_PRIVATE, -1, 0) == MAP_FAILED);
    assert(map_pages(region, 1, MAP_FIXED_NOREPLACE) == region);
    assert(nce_art_heap_unmap(region, page_size) == 0);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i)
        assert(pthread_create(&threads[i], NULL, worker, (void *)(i + 1)) == 0);
    for (unsigned i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    puts("{\"test\":\"art_high_reference_heap\",\"passed\":true}");
    return 0;
}
