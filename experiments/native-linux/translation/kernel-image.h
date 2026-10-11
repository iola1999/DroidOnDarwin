/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../nce.h"

typedef struct {
    unsigned char *base;
    NceCode *code;
} TranslationKernelImage;

/* The generated ELF layout belongs to this build. The caller must stop all
 * kernel threads before release. Load requires an empty image; a load failure keeps it empty. */
int translation_kernel_load(TranslationKernelImage *image, const char *path);
int translation_kernel_release(TranslationKernelImage *image);
/* Dual variant only, before any kernel thread starts. The caller serializes
 * this check with all other execution-memory checks. Original bytes survive. */
int translation_kernel_check_code(const TranslationKernelImage *image);
