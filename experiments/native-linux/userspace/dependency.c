/* SPDX-License-Identifier: GPL-2.0-or-later */
static _Thread_local int value = 7;
static int initialized;
__attribute__((constructor)) static void initialize(void)
{
    initialized = 19;
}
int dependency_add(int increment)
{
    value += increment;
    return value;
}
int dependency_ready(void)
{
    return initialized;
}
