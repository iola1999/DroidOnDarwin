/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static unsigned long copy_user(void *, unsigned long, unsigned long, int);
static long run_syscall(long, long *);
#include "stock-init-liblog.h"

static long fixture_pid = 1234, fixture_tid = 4242;
static struct stock_init_liblog_header header;
static unsigned char priority;
static char tag[STOCK_INIT_LIBLOG_TAG_MAX], message[STOCK_INIT_LIBLOG_PAYLOAD_MAX];
static struct stock_init_liblog_iovec vec[4];
static long args[6];

static unsigned long copy_user(void *output, unsigned long address, unsigned long bytes, int write)
{
    assert(!write);
    const struct {
        unsigned long address, bytes;
    } regions[] = {
        {(unsigned long)&header, sizeof(header)},
        {(unsigned long)&priority, sizeof(priority)},
        {(unsigned long)tag, sizeof(tag)},
        {(unsigned long)message, sizeof(message)},
        {(unsigned long)vec, sizeof(vec)},
    };
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
        if (address >= regions[i].address) {
            unsigned long offset = address - regions[i].address;
            if (offset <= regions[i].bytes && bytes <= regions[i].bytes - offset) {
                memcpy(output, (const void *)address, bytes);
                return 0;
            }
        }
    }
    return bytes;
}

static long run_syscall(long number, long *values)
{
    assert(number == 172 || number == 178);
    for (unsigned i = 0; i < 6; ++i)
        assert(values[i] == 0);
    return number == 172 ? fixture_pid : fixture_tid;
}

static void setup(void)
{
    header = (struct stock_init_liblog_header){0, 4242, 42, 700};
    fixture_pid = 1234;
    fixture_tid = 4242;
    priority = 7;
    memset(tag, 0, sizeof(tag));
    memset(message, 0, sizeof(message));
    strcpy(tag, "SurfaceFlinger");
    strcpy(message, "fixture fatal");
    vec[0] = (struct stock_init_liblog_iovec){(uint64_t)&header, 11};
    vec[1] = (struct stock_init_liblog_iovec){(uint64_t)&priority, 1};
    vec[2] = (struct stock_init_liblog_iovec){(uint64_t)tag, strlen(tag) + 1};
    vec[3] = (struct stock_init_liblog_iovec){(uint64_t)message, strlen(message) + 1};
    memset(args, 0, sizeof(args));
    args[0] = 6;
    args[1] = (long)vec;
    args[2] = 4;
}

static void capture(long number, long result)
{
    struct stock_init_liblog_header saved_header = header;
    unsigned char saved_priority = priority;
    char saved_tag[sizeof(tag)], saved_message[sizeof(message)];
    struct stock_init_liblog_iovec saved_vec[4];
    long saved_args[6];
    memcpy(saved_tag, tag, sizeof(tag));
    memcpy(saved_message, message, sizeof(message));
    memcpy(saved_vec, vec, sizeof(vec));
    memcpy(saved_args, args, sizeof(args));
    stock_init_liblog_after_syscall(number, args, result);
    assert(!memcmp(&saved_header, &header, sizeof(header)) && saved_priority == priority);
    assert(!memcmp(saved_tag, tag, sizeof(tag)) && !memcmp(saved_message, message, sizeof(message)));
    assert(!memcmp(saved_vec, vec, sizeof(vec)) && !memcmp(saved_args, args, sizeof(args)));
}

static void protocol(void)
{
    setup();
    capture(66, 11 + 1 + vec[2].bytes + vec[3].bytes);
    setup();
    memset(message, 'x', 3000);
    message[3000] = 0;
    vec[3].bytes = 3001;
    capture(66, -11);
    setup();
    fixture_tid = 65538;
    header.tid = 2;
    capture(66, -9);
    assert(stock_init_liblog_seen == 3 && stock_init_liblog_recorded == 3);
    for (unsigned test = 0; test < 21; ++test) {
        setup();
        long number = 66, result = -11;
        switch (test) {
            case 0:
                number = 64;
                break;
            case 1:
                args[2] = 3;
                break;
            case 2:
                vec[0].bytes = 12;
                break;
            case 3:
                vec[1].bytes = 2;
                break;
            case 4:
                vec[2].bytes = 0;
                break;
            case 5:
                vec[2].bytes = 129;
                break;
            case 6:
                vec[3].bytes = 0;
                break;
            case 7:
                vec[3].bytes = 4069;
                break;
            case 8:
                header.id = 2;
                break;
            case 9:
                header.id = 9;
                break;
            case 10:
                priority = 6;
                break;
            case 11:
                header.nsec = 1000000000;
                break;
            case 12:
                tag[vec[2].bytes - 1] = 'a';
                break;
            case 13:
                message[vec[3].bytes - 1] = 'a';
                break;
            case 14:
                tag[2] = 0;
                break;
            case 15:
                message[2] = 0;
                break;
            case 16:
                header.tid = 4243;
                break;
            case 17:
                vec[3].address = 1;
                break;
            case 18:
                args[1] = 1;
                break;
            case 19:
                result = 9999;
                break;
            case 20:
                vec[3].bytes = 4068;
                break;
        }
        capture(number, result);
        assert(stock_init_liblog_seen == 3 && stock_init_liblog_recorded == 3);
    }
    fprintf(stderr, "{\"fixture\":\"protocol\",\"accepted\":3,\"rejected\":21,\"guest_unchanged\":true}\n");
}

static void boundary(void)
{
    for (unsigned bytes = 2048; bytes <= 2049; ++bytes) {
        setup();
        memset(message, 'x', bytes - 1);
        message[bytes - 1] = 0;
        vec[3].bytes = bytes;
        capture(66, -11);
    }
    setup();
    header.sec = UINT32_MAX;
    header.nsec = 999999999;
    fixture_tid = header.tid = UINT16_MAX;
    args[0] = 0;
    tag[0] = 0;
    vec[2].bytes = 1;
    memset(message, 'x', 4065);
    message[4065] = 0;
    vec[3].bytes = 4066;
    capture(66, 0);
    setup();
    memset(tag, 't', sizeof(tag) - 1);
    tag[sizeof(tag) - 1] = 0;
    vec[2].bytes = sizeof(tag);
    memset(message, 'x', 3938);
    message[3938] = 0;
    vec[3].bytes = 3939;
    capture(66, -4095);
    const unsigned char ids[] = {0, 1, 3, 4, 7};
    for (unsigned i = 0; i < sizeof(ids); ++i) {
        setup();
        header.id = ids[i];
        capture(66, 1);
    }
    assert(stock_init_liblog_seen == 9 && stock_init_liblog_recorded == 9);
    for (unsigned test = 0; test < 21; ++test) {
        setup();
        long result = -11;
        switch (test) {
            case 0:
                args[0] = -1;
                break;
            case 1:
                args[2] = -1;
                break;
            case 2:
                vec[0].address = 0;
                break;
            case 3:
                vec[0].address = 1;
                break;
            case 4:
                vec[1].address = 1;
                break;
            case 5:
                vec[2].address = 1;
                break;
            case 6:
                vec[3].address = 0;
                break;
            case 7:
                vec[3].address = UINT64_MAX - 4;
                break;
            case 8:
                header.id = 5;
                break;
            case 9:
                header.id = 6;
                break;
            case 10:
                result = -4096;
                break;
            case 11:
                fixture_pid = 0;
                break;
            case 12:
                fixture_pid = -1;
                break;
            case 13:
                fixture_tid = 0;
                header.tid = 0;
                break;
            case 14:
                fixture_tid = -1;
                header.tid = UINT16_MAX;
                break;
            case 15:
                vec[1].address = UINT64_MAX;
                break;
            case 16:
                vec[3].bytes = UINT64_MAX;
                break;
            case 17:
                vec[2].bytes = UINT64_MAX;
                break;
            case 18:
                args[1] = -1;
                break;
            case 19:
                vec[3].address = (uint64_t)(message + sizeof(message) - 2);
                break;
            case 20:
                tag[0] = 0;
                vec[2].bytes = 1;
                vec[3].bytes = 4067;
                break;
        }
        capture(66, result);
        assert(stock_init_liblog_seen == 9 && stock_init_liblog_recorded == 9);
    }
    fprintf(stderr, "{\"fixture\":\"boundary\",\"accepted\":9,\"rejected\":21,\"guest_unchanged\":true}\n");
}

int main(int argc, char **argv)
{
    _Static_assert(sizeof(long) == 8, "fixture requires 64-bit long");
    assert(argc == 2);
    struct rlimit files = {4 << 20, 4 << 20}, cores = {0, 0};
    assert(!setrlimit(RLIMIT_FSIZE, &files) && !setrlimit(RLIMIT_CORE, &cores));
    if (!strcmp(argv[1], "protocol")) {
        protocol();
    } else if (!strcmp(argv[1], "boundary")) {
        boundary();
    } else {
        assert(!strcmp(argv[1], "limit"));
        setup();
        for (unsigned i = 0; i < 520; ++i)
            capture(66, -11);
        assert(stock_init_liblog_seen == 520 && stock_init_liblog_recorded == 512);
        fprintf(stderr, "{\"fixture\":\"limit\",\"accepted\":520,\"rejected\":0,\"guest_unchanged\":true}\n");
    }
    stock_init_liblog_summary();
    return fflush(stdout) == 0 ? 0 : 1;
}
