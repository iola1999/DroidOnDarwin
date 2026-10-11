/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_SESSION_H
#define NCE_SESSION_H
#include "interaction-profile.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The runner clears this callback before unmounting or stopping Linux. */
void nce_session_set_syscall(long (*call)(long, long *));
/* May block waiting for the Linux CPU; call outside the UI thread. */
int nce_session_request_stop(void);
/* Fixed diagnostic command; waits for an acknowledgement with a 60-second deadline;
 * cancellation checks run between Linux calls, which may themselves block.
 * The caller supplies an existing private output directory and serializes actions. */
int nce_session_profile(int command, const char *directory);

#ifdef __cplusplus
}
#endif
#endif
