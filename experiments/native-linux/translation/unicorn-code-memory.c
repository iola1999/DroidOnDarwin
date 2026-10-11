/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../code-memory.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

typedef struct CodeMetadata {
    struct CodeMetadata *next;
    max_align_t alignment;
    unsigned char bytes[];
} CodeMetadata;

typedef struct CodeRegion {
    NceCode code;
    CodeMetadata *metadata;
    struct CodeRegion *next;
} CodeRegion;
static CodeRegion *regions;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "Unicorn code memory:%d: %s\n", __LINE__, #c); \
            abort(); \
        } \
    } while (0)

/* Unicorn owns each cache until its engine is stopped and closed. Registry
 * updates are synchronized because separate engines may be created or closed
 * while another engine compiles. Returned aliases share the cache lifetime. */
void *nce_uc_code_alloc(size_t size)
{
    if (!size || size % 16384) {
        errno = EINVAL;
        return NULL;
    }
    CodeRegion *region = calloc(1, sizeof(*region));
    if (!region)
        return NULL;
    void *rx = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx != MAP_FAILED)
        region->code = nce_code_prepare_range(rx, size);
    if (!region->code.rw) {
        int error = errno;
        if (rx != MAP_FAILED)
            munmap(rx, size);
        free(region);
        errno = error;
        return NULL;
    }
    CHECK(!pthread_mutex_lock(&lock));
    region->next = regions;
    regions = region;
    CHECK(!pthread_mutex_unlock(&lock));
    return rx;
}

void *nce_uc_code_write(const void *address)
{
    void *result = NULL;
    CHECK(!pthread_mutex_lock(&lock));
    for (CodeRegion *region = regions; region; region = region->next) {
        uintptr_t offset = (uintptr_t)address - (uintptr_t)region->code.rx;
        if (offset < region->code.size) {
            result = region->code.rw + offset;
            break;
        }
    }
    CHECK(!pthread_mutex_unlock(&lock));
    CHECK(result);
    return result;
}

/* Metadata is ordinary data. Its lifetime ends with the containing code
 * cache reset, after Unicorn has discarded all block indexes. */
void *nce_uc_metadata_alloc(const void *address, size_t size)
{
    CHECK(size && size <= SIZE_MAX - sizeof(CodeMetadata));
    CodeMetadata *data = malloc(sizeof(*data) + size);
    CHECK(data);
    CHECK(!pthread_mutex_lock(&lock));
    CodeRegion *region = regions;
    while (region && (uintptr_t)address - (uintptr_t)region->code.rx >= region->code.size)
        region = region->next;
    CHECK(region);
    data->next = region->metadata;
    region->metadata = data;
    CHECK(!pthread_mutex_unlock(&lock));
    return data->bytes;
}

static void free_metadata(CodeRegion *region)
{
    while (region->metadata) {
        CodeMetadata *data = region->metadata;
        region->metadata = data->next;
        free(data);
    }
}

void nce_uc_metadata_reset(const void *address)
{
    CHECK(!pthread_mutex_lock(&lock));
    CodeRegion *region = regions;
    while (region && (uintptr_t)address - (uintptr_t)region->code.rx >= region->code.size)
        region = region->next;
    CHECK(region);
    free_metadata(region);
    CHECK(!pthread_mutex_unlock(&lock));
}

void nce_uc_code_free(void *address, size_t size)
{
    CHECK(!pthread_mutex_lock(&lock));
    CodeRegion **link = &regions;
    while (*link && (*link)->code.rx != address)
        link = &(*link)->next;
    CodeRegion *region = *link;
    CHECK(region && region->code.size == size);
    *link = region->next;
    CHECK(!pthread_mutex_unlock(&lock));
    CHECK(!munmap(region->code.rw, size));
    CHECK(!munmap(region->code.rx, size));
    free_metadata(region);
    free(region);
}
