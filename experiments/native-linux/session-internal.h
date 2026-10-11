/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
/* Serializes a host syscall with callback removal before kernel teardown. */
long nce_session_call(long number, long a, long b, long c, long d);
