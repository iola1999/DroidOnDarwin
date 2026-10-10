/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>

unsigned long translation_linux_setup(void *kernel, unsigned long copy_user_offset);
void *translation_linux_process(const char *path);
void translation_linux_cleanup(void);
