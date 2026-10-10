/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#include "kernel-api.h"

unsigned long translation_linux_setup(void *kernel, unsigned long copy_user_offset,
                                      unsigned long translate_offset, struct nce_translation_state *state);
void *translation_linux_process(const char *path);
void translation_linux_cleanup(void);
