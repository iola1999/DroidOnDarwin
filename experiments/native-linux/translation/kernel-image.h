/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "../nce.h"

typedef struct {
    unsigned char *base;
    NceCode *code;
} TranslationKernelImage;

/* The generated ELF layout belongs to this build. The caller must stop all
 * kernel threads before release. A failed load leaves the image empty. */
int translation_kernel_load(TranslationKernelImage *image, const char *path);
int translation_kernel_release(TranslationKernelImage *image);
