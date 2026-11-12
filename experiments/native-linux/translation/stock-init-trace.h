/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* This test copy records guest writes even after the original policy prevents
 * the host syscall task from reading the kernel ring buffer. */
static void stock_init_trace_write(long number, const long *args, long result)
{
    if (number != 64 && number != 66)
        return;
    unsigned long vectors[2] = {(unsigned long)args[1], (unsigned long)args[2]};
    unsigned count = number == 66 && args[2] > 0 ? (unsigned long)args[2] > 8 ? 8 : (unsigned)args[2] : 1;
    for (unsigned i = 0; i < count; ++i) {
        if (number == 66 && copy_user(vectors, args[1] + i * sizeof(vectors), sizeof(vectors), 0))
            return;
        unsigned char data[2048];
        size_t size = vectors[1] < sizeof(data) ? vectors[1] : sizeof(data);
        if (!size || copy_user(data, vectors[0], size, 0))
            continue;
        printf("{\"stock_init_write\":%ld,\"fd\":%ld,\"result\":%ld,\"vector\":%u,\"bytes\":%lu,\"captured\":%lu,\"hex\":\"", number, args[0], result, i, vectors[1], size);
        for (size_t j = 0; j < size; ++j)
            printf("%02x", data[j]);
        puts("\"}");
    }
}
