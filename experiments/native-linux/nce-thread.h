/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"

/* The key remains allocated for the lifetime of prepared NCE code. */
int nce_thread_key(unsigned *key);
/* Validate the platform TSD slot before executing code that reads it directly. */
int nce_thread_bind(NceState *state);
