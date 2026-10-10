/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Adapted from Leviidev/Husk src/ios-jit/husk-brk.S (GPL-2.0-or-later).
 * BreakpointJIT-compatible protocol used by StikDebug and the bundled script:
 * brk #0xf00d dispatches on x16 (0 detach, 1 prepare range), arguments in x0/x1,
 * result in x0. brk #0x69 probes the debugger. Install the trap handler first
 * so an unserviced request returns zero. See ACKNOWLEDGEMENTS.md. */

#if defined(__aarch64__) || defined(__arm64__)

.section __TEXT,__text,regular,pure_instructions

/* void *dod_brk_get_jit_mapping(void *addr, size_t len);
 * x0 = addr (pass NULL to request a fresh region), x1 = len.
 * Returns the executable region address in x0, or 0 on failure. */
.globl _dod_brk_get_jit_mapping
.p2align 2
_dod_brk_get_jit_mapping:
    mov     x16, #1              /* CMD_PREPARE_REGION */
    brk     #0xf00d
    ret

/* void dod_brk_jit_detach(void); */
.globl _dod_brk_jit_detach
.p2align 2
_dod_brk_jit_detach:
    mov     x16, #0              /* CMD_DETACH */
    brk     #0xf00d
    ret

/* uint64_t dod_brk_probe(void);
 * Returns 0xE0000069 when StikDebug is attached and servicing traps. */
.globl _dod_brk_probe
.p2align 2
_dod_brk_probe:
    brk     #0x69
    ret

#endif /* aarch64 */
