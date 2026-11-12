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

static int stock_init_trace_string(unsigned char *value, unsigned long address, size_t capacity)
{
    for (size_t i = 0; i < capacity - 1; ++i) {
        if (copy_user(value + i, address + i, 1, 0))
            return -1;
        if (!value[i])
            return 0;
    }
    return -1;
}

static void stock_init_trace_security(long number, const long *args, long result, const unsigned char *exec_path)
{
    if (!((number >= 5 && number <= 10) || number == 221 || number == 56))
        return;
    int fd_based = number == 7 || number == 10;
    unsigned long path_address = number == 56 ? args[1] : args[0];
    unsigned char path[256] = {0};
    if (number == 221)
        memcpy(path, exec_path, sizeof(path));
    else if (!fd_based && stock_init_trace_string(path, path_address, sizeof(path)))
        return;
    if (number == 56 && result >= 0 && !strstr((char *)path, "/attr/") &&
        !strstr((char *)path, "selinux/") &&
        strcmp((char *)path, "/system/bin/init") &&
        strcmp((char *)path, "/system/bin/bootstrap/linker64") &&
        strcmp((char *)path, "/dev/null") && strcmp((char *)path, "/dev/kmsg"))
        return;
    unsigned char name[128] = {0}, value[256] = {0};
    size_t bytes = 0;
    if (number >= 5 && number <= 10) {
        if (stock_init_trace_string(name, args[1], sizeof(name)))
            return;
        bytes = number <= 7 ? args[3] : result > 0 ? result
                                                   : 0;
        if (bytes > sizeof(value))
            bytes = sizeof(value);
        if (bytes && copy_user(value, args[2], bytes, 0))
            bytes = 0;
    }
    printf("{\"stock_init_security\":%ld,\"result\":%ld,\"fd\":%ld,\"path_hex\":\"",
           number, result, fd_based ? args[0] : -1L);
    for (size_t i = 0; i < strlen((char *)path); ++i)
        printf("%02x", path[i]);
    printf("\",\"name_hex\":\"");
    for (size_t i = 0; i < strlen((char *)name); ++i)
        printf("%02x", name[i]);
    printf("\",\"value_hex\":\"");
    for (size_t i = 0; i < bytes; ++i)
        printf("%02x", value[i]);
    puts("\"}");
}

static void stock_init_trace_result(long number, const long *args, long result, unsigned long pc)
{
    if (result >= 0 && number != 142)
        return;
    printf("{\"stock_init_syscall\":%ld,\"pc\":%lu,\"args\":[%lu,%lu,%lu,%lu,%lu,%lu],\"result\":%ld}\n",
           number, pc, (unsigned long)args[0], (unsigned long)args[1], (unsigned long)args[2],
           (unsigned long)args[3], (unsigned long)args[4], (unsigned long)args[5], result);
}
