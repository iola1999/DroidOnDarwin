/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#include "kernel-api.h"

unsigned long translation_linux_setup(void *kernel, unsigned long copy_user_offset,
                                      unsigned long translate_offset, struct nce_translation_state *state);
void *translation_linux_process(const char *path);
void translation_linux_cleanup(void);
/* Cancel completed or partial setup before invoking any kernel entry. */
void translation_linux_abort_setup(void);
/* Dual variant only, after debugger detach and before any kernel entry. */
int translation_linux_check_code(void);
