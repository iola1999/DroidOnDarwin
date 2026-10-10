/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"
#include <pthread.h>

enum { NCE_MAX_CPUS = 2 };

/* Called by the Linux CPU owner before handing its task to another pthread.
 * Clear a target before that pthread exits; signal delivery uses the same lock. */
void nce_cpu_target(unsigned cpu, pthread_t thread);
void nce_cpu_forget(pthread_t thread);
void nce_cpu_interrupt(unsigned mask, int signal);

/* Each CPU exclusively owns its slice until the guest enters the selected
 * branch. The caller must not schedule between this call and guest entry.
 * Returns 1 after rewriting code, 0 for a cache hit, or -1 for invalid inputs. */
int nce_cpu_resume(NceState *state, NceCode arena, unsigned cpu, unsigned cpus);
