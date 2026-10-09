/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_SESSION_H
#define NCE_SESSION_H
#ifdef __cplusplus
extern "C" {
#endif

/* The runner clears this callback before unmounting or stopping Linux. */
void nce_session_set_syscall(long (*call)(long, long *));
/* May block waiting for the Linux CPU; call outside the UI thread. */
int nce_session_request_stop(void);

#ifdef __cplusplus
}
#endif
#endif
