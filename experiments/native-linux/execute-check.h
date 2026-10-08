/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"
/* Runs only a controlled leaf function in the supplied RX range. Faults in
 * that range return -1 after recording signal, PC and address. Callers must
 * serialize checks because signal dispositions are process-wide. */
int nce_check_execute(NceCode code, int expected);
