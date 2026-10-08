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

/* Experimental full-system guest ABI: x0=destination, x1=source/fill byte,
 * x2=length, x3=operation; HLT #0x484d returns status in x8 (0=done, 1=refused).
 * Refusal preserves RAM and all argument registers. Matching QEMU is required. */
#define HUSK_BULK_INSN UINT32_C(0xd44909a0)
enum { HUSK_BULK_COPY, HUSK_BULK_MOVE, HUSK_BULK_SET, HUSK_BULK_LIMIT = 65536 };

enum { HUSK_BTYPE_SHIFT = 4, HUSK_BTYPE_MASK = 3U << HUSK_BTYPE_SHIFT,
       HUSK_MOPS_ALLOWED = 1U << 7 };

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t nzcv;
    uint32_t fpcr;
    uint32_t fpsr;
    /* Bit 0: SCTLR allows normal unaligned RAM accesses.
     * Bit 1: FP/SIMD access must trap through QEMU.
     * Bit 2: guest exposes FEAT_LSE.
     * Bit 3: EL0 lower data addresses ignore their top byte and do not use MTE.
     * Bits 4..5: pending architectural BTYPE, after the owner checked the target.
     * Direct clears these bits only after the first instruction completes.
     * Bit 6: guest exposes FEAT_CRC32.
     * Bit 7: owner permits MOPS at EL0 with MTE disabled. */
    uint32_t access_flags;
    uint64_t tpidr_el0;
    uint64_t v[32][2];
    /* QEMU's software exclusive monitor: cleaned virtual address and loaded
     * value. UINT64_MAX means cleared. Pair-load high state stays with QEMU. */
    uint64_t exclusive_address, exclusive_value;
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
    /* BLR target checked; its first instruction still needs to retire. Cached
     * calls must restore BTYPE=2 before that instruction can fault. */
    HUSK_ASSIST_CHAIN_LANDING_CALL,
    /* BR target checked without per-branch effects. Cached entries must also
     * match the source page and distinguish x16/x17 from other registers:
     * general BR derives BTYPE from the source page's guarded status.
     * The landing form preserves pending BTYPE until its first retirement. */
    HUSK_ASSIST_CHAIN_JUMP,
    HUSK_ASSIST_CHAIN_LANDING_JUMP,
    /* Completed PAC hint: only its operand register and PC changed. The owner
     * permits reuse for identical instruction/value/modifier in this run().
     * Keys, execution controls and address parameters stay fixed until return;
     * an authentication exception must leave through the owner, never return. */
    HUSK_ASSIST_PAUTH,
} HuskArm64Assist;

/* Parameters for QEMU's implementation-defined PAC hash with FEAT_PAuth2 or
 * later. The owner checks traps and refreshes keys/VA masks on every run().
 * Entries are lower A/B followed by upper A/B, selected by pointer bit 55.
 * Other PAC algorithms and failed authentication continue through assist. */
typedef struct {
    uint64_t key_lo, key_hi, mask;
    uint32_t enabled, reserved;
} HuskArm64PauthKey;
typedef struct { HuskArm64PauthKey keys[4]; } HuskArm64Pauth;

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
    /* Optional nonzero generation permits Direct to retain data mappings
     * between calls. The owner must change it after any address-space, RAM,
     * permission, watchpoint or code-dirty tracking change. Zero revalidates
     * each call. map_page must also reject aliases of all code in this call. */
    uint64_t mapping_epoch;
    const HuskArm64Pauth *pauth;
} HuskArm64Memory;

/* map_page must reject MMIO, watched pages and writable code aliases. All
 * validated pages remain accessible for the call. Zero length needs no mapping. */
bool husk_arm64_bulk(uint64_t destination, uint64_t source, uint64_t length,
                     unsigned operation, HuskArm64Memory memory);

/* CPYP/SETP prologues using normal access and default cache hints. */
static inline bool husk_arm64_mops_prologue(uint32_t word)
{
    return (word & 0xffe0fc00) == 0x1d000400 || (word & 0xffe0fc00) == 0x19c00400;
}

static inline bool husk_arm64_mops_sequence(const uint32_t code[3])
{
    uint32_t word = code[0];
    bool set = (word & 0xffe0fc00) == 0x19c00400;
    uint32_t stage = set ? 0x4000 : 0x400000;
    unsigned rd = word & 31, rn = (word >> 5) & 31, rs = (word >> 16) & 31;
    return husk_arm64_mops_prologue(word) && code[1] == (word | stage) &&
        code[2] == (word | (stage * 2)) && rd != rn && rd != rs && rn != rs &&
        rd != 31 && rn != 31 && (set || rs != 31);
}

/* The owner checks FEAT_MOPS, execution controls and disabled MTE, and supplies
 * three immutable instructions on one executable page. Success completes the
 * matching P/M/E sequence using QEMU's Option A register state. Refusal changes
 * neither memory nor state, so QEMU can handle faults and partial progress. */
bool husk_arm64_mops(HuskArm64State *state, const uint32_t instructions[3],
                     HuskArm64Memory memory);

typedef enum {
    HUSK_ARM64_BUDGET,
    HUSK_ARM64_FALLBACK,
    HUSK_ARM64_ERROR,
    /* A completed branch needs a new execute-permission check, without a TCG delay. */
    HUSK_ARM64_BRANCH,
} HuskArm64Exit;

bool husk_arm64_supported(uint32_t instruction);
/* Direct can retire this non-control instruction with precise landing faults. */
bool husk_arm64_landing_supported(uint32_t instruction);
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
