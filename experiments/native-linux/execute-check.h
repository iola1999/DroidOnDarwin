/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Runs only a controlled leaf function in the supplied RX range. Faults in
 * that range return -1 after recording signal, PC and address. Callers must
 * serialize checks because signal dispositions are process-wide. */
int nce_check_execute(NceCode code, int expected);

/* Check repeated RW updates through RX, then restore the original bytes even
 * on execution failure. No other thread may access this range during a check.
 * Calls are serialized with nce_check_execute because signal handlers are shared. */
int nce_check_code_updates(NceCode code);
#ifdef __cplusplus
}
#endif
