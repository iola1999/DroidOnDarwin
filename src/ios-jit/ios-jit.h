/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Adapted from Leviidev/Husk and AetherPS4-iOS JIT allocation code.
 * Source history and licenses: ACKNOWLEDGEMENTS.md. */
#ifndef DOD_IOS_JIT_H
#define DOD_IOS_JIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOD_EXPORT __attribute__((visibility("default")))

typedef struct JITDualMapping {
    uint8_t *rw_addr;
    uint8_t *rx_addr;
    size_t size;
} JITDualMapping;

/* Install once before JIT preparation. Unserviced JIT traps return zero;
 * unrelated signals are forwarded to the previous handler. */
DOD_EXPORT void dod_ios_jit_install_trap_handler(void);

/* Prepare a caller-owned RX range while the debugger is attached. Address and
 * size must be 16 KiB aligned. Success returns a separate RW alias; NULL rw_addr
 * reports failure. The caller must verify execution after detaching.
 * Prepared mappings can remain permanent until process exit. */
JITDualMapping dod_ios_jit_prepare_range(void *address, size_t bytes);

/* Detach after all ranges are prepared. Existing RX mappings remain valid. */
DOD_EXPORT void dod_ios_jit_detach(void);

#ifdef __cplusplus
}
#endif
#endif
