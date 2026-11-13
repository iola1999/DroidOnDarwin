/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRANSLATION_BPF_CODE_PAGE 16384UL
#define TRANSLATION_BPF_CODE_BYTES (16UL * 1024 * 1024)

/* Prepare the process-owned pool before debugger detach and kernel startup.
 * Allocation and publication are serialized internally; callers synchronize
 * execution before overwriting or freeing code. */
int translation_bpf_code_prepare(size_t bytes);
/* Before any kernel thread starts, serialized with other code checks. */
int translation_bpf_code_check(void);
/* kernel_stopped requires all kernel callers and timer callbacks to finish.
 * It permits releasing programs retained by the stopped kernel's pins or
 * attachments. Ordinary release rejects active allocations with EBUSY. */
int translation_bpf_code_release(bool kernel_stopped);
void *translation_bpf_code_alloc(unsigned long bytes);
int translation_bpf_code_free(void *address);
/* The alias has the same allocation lifetime as its RX address. */
void *translation_bpf_code_write_alias(const void *address, unsigned long bytes);
int translation_bpf_code_publish(const void *address, unsigned long bytes);
unsigned long translation_bpf_code_capacity(void);
