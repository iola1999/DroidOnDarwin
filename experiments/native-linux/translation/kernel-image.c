/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "kernel-image.h"
#include "lkl-image.h"
#ifdef NCE_TRANSLATION_DUAL_MAP
#include "../code-memory.h"
#include "../execute-check.h"
#endif
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SEGMENTS (sizeof(lkl_segments) / sizeof(*lkl_segments))

int translation_kernel_release(TranslationKernelImage *image)
{
    int result = 0;
#ifdef NCE_TRANSLATION_DUAL_MAP
    size_t cursor = 0;
    if (image->code) {
        for (size_t i = 0; i < SEGMENTS; ++i) {
            NceCode code = image->code[i];
            if (!code.rw)
                continue;
            size_t offset = (unsigned char *)code.rx - image->base;
            if (offset > cursor)
                result |= nce_code_release_range(image->base + cursor, offset - cursor, 0);
            result |= nce_code_release_range(code.rw, code.size, 1);
            result |= nce_code_release_range(code.rx, code.size, 1);
            cursor = offset + code.size;
        }
        free(image->code);
    }
    if (image->base && cursor < LKL_IMAGE_SIZE)
        result |= nce_code_release_range(image->base + cursor, LKL_IMAGE_SIZE - cursor, 0);
#else
    if (image->base && munmap(image->base, LKL_IMAGE_SIZE))
        result = -1;
#endif
    *image = (TranslationKernelImage){0};
    return result;
}

int translation_kernel_load(TranslationKernelImage *image, const char *path)
{
    if (!image || image->base || image->code) {
        errno = EINVAL;
        return -1;
    }
    FILE *input = fopen(path, "rb");
    if (!input)
        return -1;
    unsigned char *original = malloc(LKL_IMAGE_SIZE);
    if (!original) {
        fclose(input);
        return -1;
    }
    int valid = fread(original, 1, LKL_IMAGE_SIZE, input) == LKL_IMAGE_SIZE && fgetc(input) == EOF && !ferror(input);
    if (fclose(input))
        valid = 0;
    if (!valid) {
        free(original);
        errno = EIO;
        return -1;
    }
    void *base = mmap(NULL, LKL_IMAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) {
        free(original);
        return -1;
    }
    image->base = base;
    memcpy(base, original, LKL_IMAGE_SIZE);
#ifdef NCE_TRANSLATION_DUAL_MAP
    image->code = calloc(SEGMENTS, sizeof(*image->code));
    if (!image->code)
        goto failed;
    for (size_t i = 0; i < SEGMENTS; ++i) {
        size_t offset = lkl_segments[i][0], size = lkl_segments[i][1];
        if (!(lkl_segments[i][2] & PROT_EXEC))
            continue;
        image->code[i] = nce_code_prepare_range(image->base + offset, size);
        if (!image->code[i].rw)
            goto failed;
        // iOS preparation writes a short probe into each range before detach.
        memcpy(image->code[i].rw, original + offset, size);
    }
#endif
    for (size_t i = 0; i < sizeof(lkl_relocations) / sizeof(*lkl_relocations); ++i) {
        size_t offset = lkl_relocations[i][0];
        unsigned char *destination = image->base + offset;
        if (image->code) {
            for (size_t j = 0; j < SEGMENTS; ++j) {
                NceCode code = image->code[j];
                size_t local = (uintptr_t)destination - (uintptr_t)code.rx;
                if (code.rw && local < code.size) {
                    destination = code.rw + local;
                    break;
                }
            }
        }
        *(uintptr_t *)destination = (uintptr_t)image->base + lkl_relocations[i][1];
    }
    for (size_t i = 0; i < SEGMENTS; ++i) {
        if (image->code && image->code[i].rw)
            continue;
        if (mprotect(image->base + lkl_segments[i][0], lkl_segments[i][1], lkl_segments[i][2]))
            goto failed;
    }
    sys_icache_invalidate(image->base, LKL_IMAGE_SIZE);
    free(original);
    return 0;
failed: {
    int error = errno;
    free(original);
    translation_kernel_release(image);
    errno = error;
    return -1;
}
}

#ifdef NCE_TRANSLATION_DUAL_MAP
int translation_kernel_check_code(const TranslationKernelImage *image)
{
    if (!image || !image->base || !image->code)
        return -1;
    for (size_t i = 0; i < SEGMENTS; ++i)
        if (image->code[i].rw && nce_check_code_updates(image->code[i]))
            return -1;
    return 0;
}
#endif
