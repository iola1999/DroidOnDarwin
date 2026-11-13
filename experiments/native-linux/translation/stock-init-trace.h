/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef NCE_STOCK_INIT_RAMDISK
#include "stock-init-dm-abi.h"
#endif

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
#ifdef NCE_STOCK_INIT_LOG_TEXT_ONLY
        /* 无加密观察只保留解析器读取的日志前缀，省略编译产物内容。 */
        if (data[0] != '<')
            continue;
#endif
        /* 内存采样线程也写 stdout，流锁必须覆盖整条多段记录。 */
        flockfile(stdout);
        printf("{\"stock_init_write\":%ld,\"fd\":%ld,\"result\":%ld,\"vector\":%u,\"bytes\":%lu,\"captured\":%lu,\"hex\":\"", number, args[0], result, i, vectors[1], size);
        for (size_t j = 0; j < size; ++j)
            printf("%02x", data[j]);
        puts("\"}");
        funlockfile(stdout);
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

static void stock_init_trace_security(long number, const long *args, long result, const unsigned char *exec_path,
                                      unsigned long exec_index)
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
        strcmp((char *)path, "/proc/sys/vm/mmap_rnd_bits") &&
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
    flockfile(stdout);
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
    printf("\"");
    if (exec_index)
        printf(",\"exec_index\":%lu", exec_index);
    puts("}");
    funlockfile(stdout);
}

static struct stock_init_result_sample {
    unsigned long seen, recorded, pc, args[6];
    long number, result;
} stock_init_result_samples[513][2];

static void stock_init_trace_summary(void)
{
    flockfile(stdout);
    for (unsigned slot = 0; slot < 513; ++slot) {
        for (unsigned failed = 0; failed < 2; ++failed) {
            const struct stock_init_result_sample *sample = &stock_init_result_samples[slot][failed];
            if (!sample->seen)
                continue;
            if (slot == 227)
                printf("{\"stock_init_msync_summary\":true");
            else
                printf("{\"stock_init_syscall_summary\":true,\"syscall\":%ld,\"failed\":%s,\"last_syscall\":%ld",
                       slot == 512 ? -1L : (long)slot, failed ? "true" : "false", sample->number);
            printf(",\"seen\":%lu,\"recorded\":%lu,\"omitted\":%lu,\"last_pc\":%lu,\"last_args\":[%lu,%lu,%lu,%lu,%lu,%lu],\"last_result\":%ld}\n",
                   sample->seen, sample->recorded, sample->seen - sample->recorded, sample->pc,
                   sample->args[0], sample->args[1], sample->args[2],
                   sample->args[3], sample->args[4], sample->args[5], sample->result);
        }
    }
    funlockfile(stdout);
}

static void stock_init_trace_result(long number, const long *args, long result, unsigned long pc)
{
    if (result >= 0 && number != 142 && number != 63)
        return;
    /* 重复 read 和失败调用保留各类首批样本，结束后保存计数与末次参数。 */
    unsigned slot = number >= 0 && number < 512 ? (unsigned)number : 512;
    flockfile(stdout);
    struct stock_init_result_sample *sample = &stock_init_result_samples[slot][result < 0];
    unsigned long index = ++sample->seen;
    sample->number = number;
    sample->pc = pc;
    sample->result = result;
    for (unsigned i = 0; i < 6; ++i)
        sample->args[i] = (unsigned long)args[i];
    if (index <= 64 || !(index & (index - 1))) {
        ++sample->recorded;
        printf("{\"stock_init_syscall\":%ld,\"trace_index\":%lu,\"pc\":%lu,\"args\":[%lu,%lu,%lu,%lu,%lu,%lu],\"result\":%ld}\n",
               number, index, pc, (unsigned long)args[0], (unsigned long)args[1], (unsigned long)args[2],
               (unsigned long)args[3], (unsigned long)args[4], (unsigned long)args[5], result);
    }
    funlockfile(stdout);
}

static void stock_init_trace_block(long number, const long *args, long result)
{
#ifdef NCE_STOCK_INIT_RAMDISK
    if (number != 29 && number != 33 && number != 34 && number != 35 && number != 36 &&
        number != 39 && number != 40 && number != 49 && number != 51 && number != 56)
#else
    if (number != 29 && number != 33 && number != 40 && number != 56)
#endif
        return;
    unsigned char paths[3][256] = {{0}};
#ifdef NCE_STOCK_INIT_RAMDISK
    unsigned count = number == 40 ? 3 : number == 36 ? 2
                                    : number == 29   ? 0
                                                     : 1;
#else
    unsigned count = number == 40 ? 3 : number == 29 ? 0
                                                     : 1;
#endif
    for (unsigned i = 0; i < count; ++i) {
#ifdef NCE_STOCK_INIT_RAMDISK
        unsigned long address = number == 40 ? args[i] : number == 36                               ? args[i ? 2 : 0]
                                                     : number == 39 || number == 49 || number == 51 ? args[0]
                                                                                                    : args[1];
#else
        unsigned long address = number == 40 ? args[i] : args[1];
#endif
        if (address && stock_init_trace_string(paths[i], address, sizeof(paths[i])))
            return;
    }
    if ((number == 33 || number == 56) &&
#ifdef NCE_STOCK_INIT_RAMDISK
        !(!strncmp((char *)paths[0], "/dev/block", 10) && (!paths[0][10] || paths[0][10] == '/')) &&
        !(!strncmp((char *)paths[0], "/sys", 4) && (!paths[0][4] || paths[0][4] == '/')) &&
#endif
        !strstr((char *)paths[0], "loop") &&
        !strstr((char *)paths[0], "device-mapper") && !strstr((char *)paths[0], "/dev/dm-") &&
        strcmp((char *)paths[0], "/metadata") && strcmp((char *)paths[0], "/metadata/vold") &&
        strcmp((char *)paths[0], "/metadata/apex") && strcmp((char *)paths[0], "/metadata/apex/sessions"))
        return;
    long pid_args[6] = {0};
    long pid = run_syscall(172, pid_args);
    flockfile(stdout);
    printf("{\"stock_init_block\":%ld,\"pid\":%ld,\"result\":%ld,\"args\":[%lu,%lu,%lu,%lu,%lu,%lu],\"paths_hex\":[",
           number, pid, result,
           (unsigned long)args[0], (unsigned long)args[1], (unsigned long)args[2],
           (unsigned long)args[3], (unsigned long)args[4], (unsigned long)args[5]);
    for (unsigned i = 0; i < count; ++i) {
        printf("%s\"", i ? "," : "");
        for (size_t j = 0; j < strlen((char *)paths[i]); ++j)
            printf("%02x", paths[i][j]);
        putchar('"');
    }
    puts("]}");
    funlockfile(stdout);
}

#ifdef NCE_STOCK_INIT_RAMDISK
struct stock_init_trace_dm_sample {
    int active, read_result, target_read;
    struct stock_init_dm_info info;
    struct stock_init_dm_target target;
    uint32_t backing_major, backing_minor;
    uint64_t backing_start_sector;
    unsigned char backing_path[256];
};

static void stock_init_trace_dm_before(long number, const long *args, struct stock_init_trace_dm_sample *sample)
{
    sample->active = number == 29 && (uint32_t)args[1] == 0xc138fd09U; /* Linux DM_TABLE_LOAD */
    if (!sample->active)
        return;
    memset(sample, 0, sizeof(*sample));
    sample->active = 1;
    unsigned long address = (unsigned long)args[2];
    if (address > UINT64_MAX - sizeof(sample->info)) {
        sample->read_result = -22;
        return;
    }
    if (copy_user(&sample->info, address, offsetof(struct stock_init_dm_info, data), 0)) {
        sample->read_result = -14;
        return;
    }
    /* 固定 SDK 的五个 LP 均为单个 linear extent；保留超出范围的原 header。 */
    if (sample->info.target_count != 1) {
        sample->read_result = -7;
        return;
    }
    if (sample->info.version[0] != 4 || !memchr(sample->info.name, 0, sizeof(sample->info.name)) ||
        sample->info.data_start < sizeof(sample->info) || sample->info.data_start % 8 ||
        sample->info.data_start > sample->info.data_size ||
        sample->info.data_size - sample->info.data_start <= sizeof(sample->target) ||
        address > UINT64_MAX - sample->info.data_size) {
        sample->read_result = -22;
        return;
    }
    unsigned long target_address = address + sample->info.data_start;
    if (copy_user(&sample->target, target_address, sizeof(sample->target), 0)) {
        sample->read_result = -14;
        return;
    }
    sample->target_read = 1;
    if (!memchr(sample->target.type, 0, sizeof(sample->target.type))) {
        sample->read_result = -22;
        return;
    }
    if (memcmp(sample->target.type, "linear", sizeof("linear")))
        return;

    /* 其他 target 的参数可能含密钥；采样只读取 linear 参数。 */
    unsigned char parameters[sizeof(sample->backing_path) + 22];
    size_t remaining = sample->info.data_size - sample->info.data_start - sizeof(sample->target);
    size_t count = remaining < sizeof(parameters) ? remaining : sizeof(parameters);
    size_t bytes = 0;
    for (; bytes < count; ++bytes) {
        if (copy_user(parameters + bytes, target_address + sizeof(sample->target) + bytes, 1, 0)) {
            sample->read_result = -14;
            return;
        }
        if (!parameters[bytes])
            break;
    }
    if (bytes == count) {
        sample->read_result = remaining > sizeof(parameters) ? -7 : -22;
        return;
    }
    const unsigned char *cursor = parameters;
    unsigned first = 0;
    if (parameters[0] == '/') {
        const unsigned char *space = memchr(parameters, ' ', bytes);
        size_t path_bytes = space ? (size_t)(space - parameters) : 0;
        if (path_bytes <= 11 || path_bytes >= sizeof(sample->backing_path) ||
            memcmp(parameters, "/dev/block/", 11)) {
            sample->read_result = -22;
            return;
        }
        size_t start = 1;
        for (size_t i = 1; i <= path_bytes; ++i) {
            if (i < path_bytes && parameters[i] != '/')
                continue;
            size_t size = i - start;
            if (!size || (size == 1 && parameters[start] == '.') ||
                (size == 2 && parameters[start] == '.' && parameters[start + 1] == '.')) {
                sample->read_result = -22;
                return;
            }
            start = i + 1;
        }
        memcpy(sample->backing_path, parameters, path_bytes);
        cursor = space + 1;
        first = 2;
    }
    uint64_t values[3] = {0}, limits[3] = {4095, 1048575, UINT64_MAX};
    for (unsigned i = first; i < 3; ++i) {
        if (*cursor < '0' || *cursor > '9') {
            sample->read_result = -22;
            return;
        }
        do {
            unsigned digit = *cursor++ - '0';
            if (values[i] > (limits[i] - digit) / 10) {
                sample->read_result = -22;
                return;
            }
            values[i] = values[i] * 10 + digit;
        } while (*cursor >= '0' && *cursor <= '9');
        unsigned char delimiter = i == 0 ? ':' : i == 1 ? ' '
                                                        : 0;
        if (*cursor != delimiter) {
            sample->read_result = -22;
            return;
        }
        if (delimiter)
            ++cursor;
    }
    sample->backing_major = (uint32_t)values[0];
    sample->backing_minor = (uint32_t)values[1];
    sample->backing_start_sector = values[2];
}

static void stock_init_trace_dm_after(const struct stock_init_trace_dm_sample *sample, const long *args, long result)
{
    if (!sample->active)
        return;
    long pid_args[6] = {0};
    long pid = run_syscall(172, pid_args);
    flockfile(stdout);
    printf("{\"stock_init_dm_table_load\":true,\"pid\":%ld,\"fd\":%ld,\"result\":%ld,\"read_result\":%d,\"version\":[%u,%u,%u],\"data_size\":%u,\"data_start\":%u,\"target_count\":%u,\"flags\":%u,\"dev\":%llu,\"name_hex\":\"",
           pid, args[0], result, sample->read_result,
           sample->info.version[0], sample->info.version[1], sample->info.version[2],
           sample->info.data_size, sample->info.data_start, sample->info.target_count, sample->info.flags, (unsigned long long)sample->info.dev);
    for (size_t i = 0; i < sizeof(sample->info.name) && sample->info.name[i]; ++i)
        printf("%02x", (unsigned char)sample->info.name[i]);
    printf("\",\"targets\":[");
    if (sample->target_read) {
        printf("{\"type_hex\":\"");
        for (size_t i = 0; i < sizeof(sample->target.type) && sample->target.type[i]; ++i)
            printf("%02x", (unsigned char)sample->target.type[i]);
        printf("\",\"sector_start\":%llu,\"length\":%llu", (unsigned long long)sample->target.sector_start,
               (unsigned long long)sample->target.length);
        if (!sample->read_result && !memcmp(sample->target.type, "linear", sizeof("linear"))) {
            if (sample->backing_path[0]) {
                printf(",\"backing_path_hex\":\"");
                for (size_t i = 0; sample->backing_path[i]; ++i)
                    printf("%02x", sample->backing_path[i]);
                printf("\"");
            } else {
                printf(",\"backing_major\":%u,\"backing_minor\":%u", sample->backing_major, sample->backing_minor);
            }
            printf(",\"backing_start_sector\":%llu", (unsigned long long)sample->backing_start_sector);
        }
        putchar('}');
    }
    puts("]}");
    funlockfile(stdout);
}
#endif
