/* SPDX-License-Identifier: Apache-2.0 */
#include "reference_heap.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define HEAP_SIZE (UINT64_C(1) << 32)
#define MIN_PAGE_SIZE 4096

uintptr_t nce_art_reference_base;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t page_size;
static uint8_t occupied[HEAP_SIZE / MIN_PAGE_SIZE / 8];

void nce_art_invalid_reference(const void *pointer)
{
    fprintf(stderr, "NCE ART: reference outside heap: pointer=%p base=%p caller=%p\n",
            pointer, (void *)nce_art_reference_base, __builtin_return_address(0));
    abort();
}

static int used(size_t page) { return (occupied[page / 8] >> (page % 8)) & 1; }
static void set_used(size_t page, int value)
{
    unsigned mask = 1u << (page % 8);
    occupied[page / 8] = (occupied[page / 8] & ~mask) | (value ? mask : 0);
}

int nce_art_heap_init(void)
{
    pthread_mutex_lock(&heap_lock);
    if (!nce_art_reference_base) {
        long size = sysconf(_SC_PAGESIZE);
        if (size < MIN_PAGE_SIZE || (size & (size - 1))) {
            pthread_mutex_unlock(&heap_lock);
            errno = EINVAL;
            return -1;
        }
        void *base = mmap(NULL, HEAP_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            pthread_mutex_unlock(&heap_lock);
            return -1;
        }
        page_size = (size_t)size;
        nce_art_reference_base = (uintptr_t)base;
        set_used(0, 1);
    }
    pthread_mutex_unlock(&heap_lock);
    return 0;
}

int nce_art_heap_contains(const void *address, size_t length)
{
    uintptr_t offset = (uintptr_t)address - nce_art_reference_base;
    return nce_art_reference_base && offset < HEAP_SIZE && length <= HEAP_SIZE - offset;
}

void *nce_art_heap_map(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    if (!page_size || !length || length % page_size || length > HEAP_SIZE - page_size ||
        (uintptr_t)address % page_size ||
        (!address && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) ||
        (address && (!nce_art_heap_contains(address, length) ||
                     (uintptr_t)address == nce_art_reference_base))) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    pthread_mutex_lock(&heap_lock);
    size_t count = length / page_size;
    size_t first = address ? ((uintptr_t)address - nce_art_reference_base) / page_size : 1;
    if (!address) {
        size_t consecutive = 0, page;
        for (page = 1; page < HEAP_SIZE / page_size; ++page) {
            consecutive = used(page) ? 0 : consecutive + 1;
            if (consecutive == count) { first = page + 1 - count; break; }
        }
        if (page == HEAP_SIZE / page_size) {
            pthread_mutex_unlock(&heap_lock);
            errno = ENOMEM;
            return MAP_FAILED;
        }
    } else if (!(flags & MAP_FIXED) || (flags & MAP_FIXED_NOREPLACE)) {
        for (size_t page = first; page < first + count; ++page) {
            if (used(page)) {
                pthread_mutex_unlock(&heap_lock);
                errno = EEXIST;
                return MAP_FAILED;
            }
        }
    }
    /* 整个区域已归本分配器所有；固定映射只替换其中经过检查的页面。 */
    void *result = mmap((void *)(nce_art_reference_base + first * page_size), length,
                        prot, (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED, fd, offset);
    if (result != MAP_FAILED) {
        for (size_t page = first; page < first + count; ++page) set_used(page, 1);
    }
    pthread_mutex_unlock(&heap_lock);
    return result;
}

int nce_art_heap_unmap(void *address, size_t length)
{
    if (!page_size || !length || length % page_size || (uintptr_t)address % page_size ||
        !nce_art_heap_contains(address, length) || (uintptr_t)address == nce_art_reference_base) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&heap_lock);
    /* 保留地址所有权，防止其他分配器占用已释放的堆范围。 */
    void *result = mmap(address, length, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result != MAP_FAILED) {
        size_t first = ((uintptr_t)address - nce_art_reference_base) / page_size;
        for (size_t page = first; page < first + length / page_size; ++page) set_used(page, 0);
    }
    pthread_mutex_unlock(&heap_lock);
    return result == MAP_FAILED ? -1 : 0;
}
