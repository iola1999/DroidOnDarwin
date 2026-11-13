/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bpf-code-memory.h"
#include "../code-memory.h"
#include "../execute-check.h"
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static NceCode pool;
/* A start stores its page count; continuation pages store SIZE_MAX. */
static size_t *allocations;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const uint32_t fault_instruction = 0xd4200000;

static void publish(size_t offset, size_t bytes)
{
    sys_dcache_flush(pool.rw + offset, bytes);
    sys_icache_invalidate(pool.rx + offset, bytes);
}

static void fill(size_t offset, size_t bytes, uint32_t instruction)
{
    for (size_t i = 0; i < bytes; i += sizeof(instruction))
        memcpy(pool.rw + offset + i, &instruction, sizeof(instruction));
    publish(offset, bytes);
}

int translation_bpf_code_prepare(size_t bytes)
{
    if (!bytes || bytes % TRANSLATION_BPF_CODE_PAGE) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&lock);
    if (pool.rx || pool.rw) {
        pthread_mutex_unlock(&lock);
        errno = EBUSY;
        return -1;
    }
    size_t *records = calloc(bytes / TRANSLATION_BPF_CODE_PAGE, sizeof(*records));
    void *address = records ? mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0) : MAP_FAILED;
    NceCode code = address == MAP_FAILED ? (NceCode){0} : nce_code_prepare_range(address, bytes);
    if (!code.rw) {
        int error = errno;
        if (address != MAP_FAILED)
            munmap(address, bytes);
        free(records);
        pthread_mutex_unlock(&lock);
        errno = error;
        return -1;
    }
    pool = code;
    allocations = records;
    fill(0, pool.size, fault_instruction);
    pthread_mutex_unlock(&lock);
    return 0;
}

unsigned long translation_bpf_code_capacity(void)
{
    pthread_mutex_lock(&lock);
    unsigned long result = pool.size;
    pthread_mutex_unlock(&lock);
    return result;
}

int translation_bpf_code_check(void)
{
    pthread_mutex_lock(&lock);
    if (!pool.rx || !pool.rw) {
        pthread_mutex_unlock(&lock);
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < pool.size / TRANSLATION_BPF_CODE_PAGE; ++i) {
        if (allocations[i]) {
            pthread_mutex_unlock(&lock);
            errno = EBUSY;
            return -1;
        }
    }
    int result = nce_check_code_updates(pool);
    pthread_mutex_unlock(&lock);
    return result;
}

void *translation_bpf_code_alloc(unsigned long bytes)
{
    if (!bytes || bytes > SIZE_MAX - (TRANSLATION_BPF_CODE_PAGE - 1)) {
        errno = EINVAL;
        return NULL;
    }
    size_t count = (bytes + TRANSLATION_BPF_CODE_PAGE - 1) / TRANSLATION_BPF_CODE_PAGE;
    pthread_mutex_lock(&lock);
    size_t total = pool.rx && pool.rw ? pool.size / TRANSLATION_BPF_CODE_PAGE : 0;
    size_t start = 0, available = 0;
    void *result = NULL;
    for (size_t i = 0; i < total; ++i) {
        if (allocations[i]) {
            available = 0;
            start = i + 1;
        } else if (++available == count) {
            allocations[start] = count;
            for (size_t j = 1; j < count; ++j)
                allocations[start + j] = SIZE_MAX;
            result = pool.rx + start * TRANSLATION_BPF_CODE_PAGE;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    if (!result)
        errno = ENOMEM;
    return result;
}

/* The pack allocator can publish a small chunk inside a host allocation. */
static int range_offset(const void *address, size_t bytes, size_t *offset)
{
    if (!address || !bytes || !pool.rx || !pool.rw)
        return -1;
    size_t local = (uintptr_t)address - (uintptr_t)pool.rx;
    if (local >= pool.size || bytes > pool.size - local)
        return -1;
    size_t page = local / TRANSLATION_BPF_CODE_PAGE;
    while (page && allocations[page] == SIZE_MAX)
        --page;
    size_t count = allocations[page];
    size_t end = (page + count) * TRANSLATION_BPF_CODE_PAGE;
    if (!count || count == SIZE_MAX || bytes > end - local)
        return -1;
    *offset = local;
    return 0;
}

void *translation_bpf_code_write_alias(const void *address, unsigned long bytes)
{
    pthread_mutex_lock(&lock);
    size_t offset;
    void *result = !range_offset(address, bytes, &offset) ? pool.rw + offset : NULL;
    pthread_mutex_unlock(&lock);
    if (!result)
        errno = EINVAL;
    return result;
}

int translation_bpf_code_publish(const void *address, unsigned long bytes)
{
    pthread_mutex_lock(&lock);
    size_t offset;
    int result = range_offset(address, bytes, &offset);
    if (!result)
        publish(offset, bytes);
    pthread_mutex_unlock(&lock);
    if (result)
        errno = EINVAL;
    return result;
}

int translation_bpf_code_free(void *address)
{
    if (!address)
        return 0;
    pthread_mutex_lock(&lock);
    size_t offset = (uintptr_t)address - (uintptr_t)pool.rx;
    size_t page = offset / TRANSLATION_BPF_CODE_PAGE;
    if (!pool.rx || offset >= pool.size || offset % TRANSLATION_BPF_CODE_PAGE ||
        !allocations[page] || allocations[page] == SIZE_MAX) {
        pthread_mutex_unlock(&lock);
        errno = EINVAL;
        return -1;
    }
    size_t count = allocations[page];
    fill(offset, count * TRANSLATION_BPF_CODE_PAGE, fault_instruction);
    memset(allocations + page, 0, count * sizeof(*allocations));
    pthread_mutex_unlock(&lock);
    return 0;
}

int translation_bpf_code_release(bool kernel_stopped)
{
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < pool.size / TRANSLATION_BPF_CODE_PAGE; ++i) {
        if (allocations[i] && !kernel_stopped) {
            pthread_mutex_unlock(&lock);
            errno = EBUSY;
            return -1;
        }
    }
    int result = 0;
    if (pool.rw) {
        result = nce_code_release_range(pool.rw, pool.size, 1);
        if (!result)
            pool.rw = NULL;
    }
    if (!result && pool.rx) {
        result = nce_code_release_range(pool.rx, pool.size, 1);
        if (!result)
            pool.rx = NULL;
    }
    if (!result) {
        free(allocations);
        allocations = NULL;
        pool = (NceCode){0};
    }
    pthread_mutex_unlock(&lock);
    return result;
}
