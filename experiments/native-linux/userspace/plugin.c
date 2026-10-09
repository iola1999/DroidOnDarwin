/* SPDX-License-Identifier: GPL-2.0-or-later */
static _Thread_local int value = 100;
static int initialized;
__attribute__((constructor)) static void initialize(void) { initialized = 23; }
int plugin_add(int increment) { value += increment; return initialized == 23 ? value : -1; }
