/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce-thread.h"
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t state_key;
static int key_error;

static int validate_slot(void *expected)
{
    uintptr_t base;
    __asm__("mrs %0, tpidrro_el0" : "=r"(base));
    if (!base || (base & 7) || base > UINTPTR_MAX - state_key * sizeof(void *))
        return ENOTSUP;
    uintptr_t value = 0;
    vm_size_t copied = 0;
    /* A changed platform layout must fail before a generated LDR dereferences it. */
    kern_return_t result = vm_read_overwrite(mach_task_self(), base + state_key * sizeof(void *),
                                             sizeof(value), (vm_address_t)&value, &copied);
    return result == KERN_SUCCESS && copied == sizeof(value) &&
                   value == (uintptr_t)expected && pthread_getspecific(state_key) == expected
               ? 0
               : ENOTSUP;
}

static void create_key(void)
{
    key_error = pthread_key_create(&state_key, NULL);
    if (key_error)
        return;
    /* ARM64 LDR unsigned immediate has a 12-bit, eight-byte-scaled offset. */
    if (state_key >= 4096) {
        key_error = ENOTSUP;
    } else {
        key_error = pthread_setspecific(state_key, &state_key);
        if (!key_error)
            key_error = validate_slot(&state_key);
        int cleared = pthread_setspecific(state_key, NULL);
        if (!key_error)
            key_error = cleared;
    }
    if (key_error)
        pthread_key_delete(state_key);
}

int nce_thread_key(unsigned *key)
{
    int result = pthread_once(&once, create_key);
    if (result || key_error)
        return result ? result : key_error;
    *key = (unsigned)state_key;
    return 0;
}

int nce_thread_bind(NceState *state)
{
    unsigned key;
    int result = nce_thread_key(&key);
    if (!result)
        result = pthread_setspecific(key, state);
    if (!result)
        result = validate_slot(state);
    return result;
}
