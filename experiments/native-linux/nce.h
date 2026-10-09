/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "guest-registers.h"

/* Serialized guest execution ABI. x18 remains owned by Darwin. */
typedef struct NceState NceState;
struct NceState {
    struct pt_regs regs;
    uint64_t host_sp, host_x18, resume;
    uint32_t instruction, padding;
    void (*syscall)(NceState *);
    void *opaque;
    uint64_t done, tls, calls, tls_accesses;
    NceState *self;
    NceState *(*event)(NceState *);
};

typedef struct {
    unsigned char *rw, *rx;
    size_t size;
} NceCode;

/* Loads the controlled, relocation-free fixture. Both mappings refer to the
 * same storage; the caller owns executable-memory authorization and lifetime. */
int nce_prepare(NceCode, NceState *, const void *, size_t, size_t entry);
/* The active slot must outlive the code and track every guest task switch. */
int nce_prepare_shared(NceCode, NceState *, NceState **active, const void *, size_t, size_t entry);
void nce_enter(NceState *);
void nce_gate(void);
void nce_event_gate(void);
void nce_restore_stack(void);
NceState *nce_dispatch(NceState *);
NceState *nce_event_dispatch(NceState *);
void nce_sync_code(NceCode);
/* Emits one resume entry or one SVC/TLS stub into caller-owned storage. The
 * caller supplies a distinct stub for each instruction and flushes afterwards. */
int nce_write_entry(NceCode stub, uintptr_t entry);
int nce_write_patch(uint32_t *write_address, uintptr_t execute_address,
                    NceCode stub, NceState **active, uint32_t instruction);
