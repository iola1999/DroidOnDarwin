/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_ARM64_EXEC_H
#define HUSK_ARM64_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HuskArm64Exec HuskArm64Exec;

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t nzcv;
    uint32_t fpcr;
    uint32_t fpsr;
    /* Bit 0: SCTLR allows normal unaligned RAM accesses.
     * Bit 1: FP/SIMD access must trap through QEMU.
     * Bit 2: guest exposes FEAT_LSE. */
    uint32_t access_flags;
    uint64_t tpidr_el0;
    uint64_t v[32][2];
} HuskArm64State;

typedef enum {
    HUSK_ASSIST_DECLINED,
    HUSK_ASSIST_CONTINUE,
    /* The owner must validate the destination, including branch landing rules. */
    HUSK_ASSIST_BRANCH,
    /* Completed branch with landing checks resolved; map_code may extend the
     * current slice. The destination uses a budget-checked branch entry. */
    HUSK_ASSIST_CHAIN,
    /* BLR landing checks completed without per-call effects such as PAC.
     * Within this slice, the same target may reuse the checked destination. */
    HUSK_ASSIST_CHAIN_CALL,
} HuskArm64Assist;

typedef struct {
    void *opaque;
    /* Failure must leave guest memory unchanged. Never unwind through JIT code. */
    bool (*read)(void *, uint64_t, void *, size_t);
    bool (*write)(void *, uint64_t, const void *, size_t);
    /* Optional: validate an entire 4 KiB RAM page for this slice, including
     * address tags under the guest's current MMU configuration. No MMIO or
     * watched pages; writes must reject physical aliases of the current code.
     * The mapping and permissions must remain valid until run() returns. */
    void *(*map_page)(void *, uint64_t page_address, bool write);
    /* Optional instruction assist, called after native code has returned.
     * Success must advance PC and preserve mappings and this code page.
     * Failure leaves state unchanged. Exceptions require the owner to copy
     * the complete state before leaving run() through its CPU exception path. */
    HuskArm64Assist (*assist)(void *, HuskArm64State *, uint32_t instruction);
    /* Optional: extend this slice to another executable 4 KiB page. The owner
     * must keep every returned code page immutable until run() returns, reject
     * writes through physical aliases, and preserve all earlier data mappings.
     * NULL ends the slice before executing the destination. */
    const void *(*map_code)(void *, uint64_t page_address);
} HuskArm64Memory;

typedef enum {
    HUSK_ARM64_BUDGET,
    HUSK_ARM64_FALLBACK,
    HUSK_ARM64_ERROR,
    /* A completed branch needs a new execute-permission check, without a TCG delay. */
    HUSK_ARM64_BRANCH,
} HuskArm64Exit;

bool husk_arm64_supported(uint32_t instruction);
HuskArm64Exec *husk_arm64_create(void);
void husk_arm64_destroy(HuskArm64Exec *context);

/*
 * Starts execution from an immutable 4 KiB code page. Optional map_code extends
 * the slice across pages. The owner validates execute permission and supplies
 * current contents on every entry. Data callbacks enforce the current guest
 * exception level's permissions and reject writes aliasing every visited code page.
 * FALLBACK leaves PC at an instruction for the owner to execute, including the
 * faulting instruction after a failed data access. Budget is a soft instruction
 * bound checked between blocks, not an exact instruction count. Contexts are
 * thread-confined. HUSK_ARM64_BACKEND=direct selects the macOS direct backend;
 * its data accesses require map_page, otherwise they return FALLBACK. The owner
 * must describe FP access in access_flags, disable SVE/SME, and preserve
 * guest exception state.
 * Unsupported FPCR modes return FALLBACK before changing guest state.
 */
HuskArm64Exit husk_arm64_run(HuskArm64Exec *context, HuskArm64State *state,
                           uint64_t page_address, const void *page_bytes,
                           HuskArm64Memory memory, uint64_t budget);

#ifdef __cplusplus
}
#endif
#endif
