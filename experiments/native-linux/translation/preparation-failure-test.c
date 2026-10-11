/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../code-memory.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

extern NceCode real_prepare_range(void *, size_t);
extern int real_release_range(void *, size_t, int);
static struct {
    NceCode code;
    unsigned released;
} prepared[16];
static unsigned attempts, count;

/* Linked only into the failure regression executable. */
NceCode nce_code_prepare_range(void *address, size_t size)
{
    const char *selected = getenv("NCE_TEST_PREPARATION_FAIL_AT");
    if (++attempts == (unsigned)(selected ? atoi(selected) : 0)) {
        errno = EACCES;
        return (NceCode){0};
    }
    NceCode code = real_prepare_range(address, size);
    if (code.rw) {
        if (count == sizeof(prepared) / sizeof(*prepared))
            abort();
        prepared[count++].code = code;
    }
    return code;
}

int nce_code_release_range(void *address, size_t size, int is_prepared)
{
    int result = real_release_range(address, size, is_prepared);
    if (!result) {
        for (unsigned i = 0; i < count; ++i) {
            NceCode code = prepared[i].code;
            if (code.size != size)
                continue;
            if (code.rw == address)
                prepared[i].released |= 1;
            if (code.rx == address)
                prepared[i].released |= 2;
        }
    }
    return result;
}

__attribute__((destructor)) static void check_release(void)
{
    for (unsigned i = 0; i < count; ++i)
        if (prepared[i].released != 3)
            abort();
    printf("{\"test\":\"partial_preparation_cleanup\",\"passed\":true,\"attempts\":%u,\"prepared\":%u}\n", attempts, count);
}
