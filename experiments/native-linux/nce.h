/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Single-threaded probe ABI. x18 remains owned by Darwin. */
typedef struct NceState NceState;
struct NceState {
    uint64_t x[31], sp, pc, nzcv, fpcr, fpsr;
    uint64_t vector[32][2];
    uint64_t host_sp, host_x18, resume;
    uint32_t instruction, padding;
    void (*syscall)(NceState *);
    void *opaque;
    uint64_t done, tls, calls, tls_accesses;
};

typedef struct {
    unsigned char *rw, *rx;
    size_t size;
} NceCode;

/* Loads the controlled, relocation-free fixture. Both mappings refer to the
 * same storage; the caller owns executable-memory authorization and lifetime. */
int nce_prepare(NceCode, NceState *, const void *, size_t, size_t entry);
void nce_enter(NceState *);
void nce_gate(void);
NceState *nce_dispatch(NceState *);
void nce_sync_code(NceCode);
