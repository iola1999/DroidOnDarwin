/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* SDK36 liblog 使用四个 writev 向量；copy_user 和 run_syscall 由执行入口提供。 */
struct __attribute__((packed)) stock_init_liblog_header {
    uint8_t id;
    uint16_t tid;
    uint32_t sec, nsec;
};
struct stock_init_liblog_iovec {
    uint64_t address, bytes;
};
_Static_assert(sizeof(struct stock_init_liblog_header) == 11, "liblog packed header");
_Static_assert(sizeof(struct stock_init_liblog_iovec) == 16, "ARM64 Linux iovec");
enum {
    STOCK_INIT_LIBLOG_PAYLOAD_MAX = 4068,
    STOCK_INIT_LIBLOG_TAG_MAX = 128,
    STOCK_INIT_LIBLOG_CAPTURE_MAX = 2048,
    STOCK_INIT_LIBLOG_RECORD_MAX = 512
};
static unsigned long stock_init_liblog_seen, stock_init_liblog_recorded;

static void stock_init_liblog_after_syscall(long number, const long *args, long write_result)
{
    if (number != 66 || args[0] < 0 || args[2] != 4)
        return;
    struct stock_init_liblog_iovec vec[4];
    if (copy_user(vec, args[1], sizeof(vec), 0) || vec[0].bytes != 11 || vec[1].bytes != 1 ||
        !vec[2].bytes || vec[2].bytes > STOCK_INIT_LIBLOG_TAG_MAX ||
        !vec[3].bytes || vec[3].bytes > STOCK_INIT_LIBLOG_PAYLOAD_MAX ||
        1 + vec[2].bytes + vec[3].bytes > STOCK_INIT_LIBLOG_PAYLOAD_MAX)
        return;
    for (unsigned i = 0; i < 4; ++i)
        if (!vec[i].address || vec[i].address > UINT64_MAX - vec[i].bytes)
            return;
    const uint64_t payload = 1 + vec[2].bytes + vec[3].bytes;
    if (write_result < -4095 || (write_result >= 0 && (uint64_t)write_result > 11 + payload))
        return;
    struct stock_init_liblog_header header;
    unsigned char priority, tag[STOCK_INIT_LIBLOG_TAG_MAX], message[STOCK_INIT_LIBLOG_PAYLOAD_MAX];
    if (copy_user(&header, vec[0].address, sizeof(header), 0) ||
        copy_user(&priority, vec[1].address, 1, 0) || priority != 7 ||
        header.id > 7 || header.id == 2 || header.id == 5 || header.id == 6 ||
        header.nsec >= 1000000000U ||
        copy_user(tag, vec[2].address, vec[2].bytes, 0) ||
        copy_user(message, vec[3].address, vec[3].bytes, 0))
        return;
    if (tag[vec[2].bytes - 1] || message[vec[3].bytes - 1] ||
        memchr(tag, 0, vec[2].bytes - 1) || memchr(message, 0, vec[3].bytes - 1))
        return;
    long identity[6] = {0};
    const long pid = run_syscall(172, identity), tid = run_syscall(178, identity);
    /* packed tid 按 uint16_t 保存 Linux gettid 的值。 */
    if (pid <= 0 || tid <= 0 || header.tid != (uint16_t)tid)
        return;
    flockfile(stdout);
    ++stock_init_liblog_seen;
    if (stock_init_liblog_recorded < STOCK_INIT_LIBLOG_RECORD_MAX) {
        ++stock_init_liblog_recorded;
        const uint64_t captured = vec[3].bytes < STOCK_INIT_LIBLOG_CAPTURE_MAX ? vec[3].bytes : STOCK_INIT_LIBLOG_CAPTURE_MAX;
        printf("{\"stock_init_liblog_fatal\":true,\"log_index\":%lu,\"pid\":%ld,\"tid\":%ld,\"header_tid\":%u,\"log_id\":%u,\"sec\":%u,\"nsec\":%u,\"fd\":%ld,\"write_result\":%ld,\"tag_bytes\":%llu,\"message_bytes\":%llu,\"captured_bytes\":%llu,\"tag_hex\":\"",
               stock_init_liblog_seen, pid, tid, header.tid, header.id, header.sec, header.nsec,
               args[0], write_result, (unsigned long long)vec[2].bytes,
               (unsigned long long)vec[3].bytes, (unsigned long long)captured);
        for (uint64_t i = 0; i < vec[2].bytes; ++i)
            printf("%02x", tag[i]);
        printf("\",\"message_hex\":\"");
        for (uint64_t i = 0; i < captured; ++i)
            printf("%02x", message[i]);
        puts("\"}");
    }
    funlockfile(stdout);
}

/* 执行入口停止全部 guest 线程后再输出这一份汇总。 */
static void stock_init_liblog_summary(void)
{
    flockfile(stdout);
    printf("{\"stock_init_liblog_summary\":true,\"seen\":%lu,\"recorded\":%lu,\"omitted\":%lu,\"capture_limit_bytes\":%u,\"record_limit\":%u}\n",
           stock_init_liblog_seen, stock_init_liblog_recorded,
           stock_init_liblog_seen - stock_init_liblog_recorded,
           STOCK_INIT_LIBLOG_CAPTURE_MAX, STOCK_INIT_LIBLOG_RECORD_MAX);
    funlockfile(stdout);
}
