/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

void translation_elf_initialize(unsigned long user_base);
unsigned long translation_elf_layout(const char *path, unsigned long offset, const void *header, const void *phdrs);
unsigned long translation_elf_offset(unsigned long address);
void translation_elf_cleanup(void);
