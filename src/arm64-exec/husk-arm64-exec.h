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
} HuskArm64State;

typedef struct {
    void *opaque;
    /* Failure must leave guest memory unchanged. Never unwind through JIT code. */
    bool (*read)(void *, uint64_t, void *, size_t);
    bool (*write)(void *, uint64_t, const void *, size_t);
    /* Optional: validate an entire 4 KiB RAM page for this slice. No MMIO or
     * watched pages; writes must reject physical aliases of the current code.
     * The mapping and permissions must remain valid until run() returns. */
    void *(*map_page)(void *, uint64_t page_address, bool write);
} HuskArm64Memory;

typedef enum {
    HUSK_ARM64_BUDGET,
    HUSK_ARM64_FALLBACK,
    HUSK_ARM64_ERROR,
} HuskArm64Exit;

bool husk_arm64_supported(uint32_t instruction);
HuskArm64Exec *husk_arm64_create(void);
void husk_arm64_destroy(HuskArm64Exec *context);

/*
 * Runs integer EL0 code from one immutable 4 KiB code page. The owner validates
 * execute permission and supplies current page contents on every entry. Data
 * callbacks enforce guest permissions and reject writes aliasing this code page.
 * FALLBACK leaves PC at an instruction for the owner to execute, including the
 * faulting instruction after a failed data access. Budget is a soft instruction
 * bound checked between blocks, not an exact instruction count. Contexts are
 * thread-confined. HUSK_ARM64_BACKEND=direct selects the macOS direct backend;
 * its data accesses require map_page, otherwise they return FALLBACK.
 */
HuskArm64Exit husk_arm64_run(HuskArm64Exec *context, HuskArm64State *state,
                           uint64_t page_address, const void *page_bytes,
                           HuskArm64Memory memory, uint64_t budget);

#ifdef __cplusplus
}
#endif
#endif
