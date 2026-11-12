/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/bpf.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(test) \
    do { \
        if (!(test)) { \
            fprintf(stderr, "ringbuf check line=%d errno=%d\n", __LINE__, errno); \
            exit(90); \
        } \
    } while (0)
#define PAGE 4096
#define PAYLOAD 32
#define RECORD (PAYLOAD + 8)
#define DISCARD (1U << 30)
#define BUSY (1U << 31)
#define I(code_, dst_, src_, off_, imm_) ((struct bpf_insn){.code = (code_), .dst_reg = (dst_), .src_reg = (src_), .off = (off_), .imm = (imm_)})
#define MOVI(r, v) I(BPF_ALU64 | BPF_MOV | BPF_K, r, 0, 0, v)
#define MOVR(d, s) I(BPF_ALU64 | BPF_MOV | BPF_X, d, s, 0, 0)
#define CALL(fn) I(BPF_JMP | BPF_CALL, 0, 0, 0, fn)
#define EXIT I(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

static int bpf(int command, union bpf_attr *attr)
{
    return syscall(SYS_bpf, command, attr, sizeof(*attr));
}

static int map_create(unsigned type, unsigned bytes)
{
    union bpf_attr attr = {.map_type = type, .max_entries = bytes};
    if (type == BPF_MAP_TYPE_ARRAY) {
        attr.key_size = 4;
        attr.value_size = 4;
    }
    return bpf(BPF_MAP_CREATE, &attr);
}

static int load(struct bpf_insn *instructions, unsigned count)
{
    char log[65536] = {0};
    union bpf_attr attr = {.prog_type = BPF_PROG_TYPE_SOCKET_FILTER, .insn_cnt = count, .insns = (uintptr_t)instructions, .license = (uintptr_t)"GPL", .log_buf = (uintptr_t)log, .log_size = sizeof(log), .log_level = 1};
    int fd = bpf(BPF_PROG_LOAD, &attr);
    if (fd < 0)
        fprintf(stderr, "BPF load errno=%d: %s\n", errno, log);
    CHECK(fd >= 0);
    return fd;
}

static int producer(int map, int discard)
{
    struct bpf_insn instructions[32];
    unsigned n = 0;
    instructions[n++] = I(BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, map);
    instructions[n++] = I(0, 0, 0, 0, 0);
    instructions[n++] = MOVI(2, PAYLOAD);
    instructions[n++] = MOVI(3, 0);
    instructions[n++] = CALL(BPF_FUNC_ringbuf_reserve);
    unsigned branch = n;
    instructions[n++] = I(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 0, 0);
    for (unsigned i = 0; i < PAYLOAD / 4; i++)
        instructions[n++] = I(BPF_ST | BPF_MEM | BPF_W, 0, 0, i * 4, 0x12345600 + i);
    instructions[n++] = MOVR(1, 0);
    instructions[n++] = MOVI(2, 0);
    instructions[n++] = CALL(discard ? BPF_FUNC_ringbuf_discard : BPF_FUNC_ringbuf_submit);
    instructions[n++] = MOVI(0, 0);
    instructions[n++] = EXIT;
    instructions[branch].off = n - branch - 1;
    instructions[n++] = MOVI(0, 1);
    instructions[n++] = EXIT;
    return load(instructions, n);
}

static unsigned run(int program, unsigned repeat)
{
    unsigned char packet[64] = {0};
    union bpf_attr attr = {0};
    attr.test.prog_fd = program;
    attr.test.data_in = (uintptr_t)packet;
    attr.test.data_size_in = sizeof(packet);
    attr.test.repeat = repeat;
    CHECK(bpf(BPF_PROG_TEST_RUN, &attr) == 0);
    return attr.test.retval;
}

static void rejected_write(void *mapping, size_t bytes)
{
    errno = 0;
    CHECK(mprotect(mapping, bytes, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
}

static void kernel_producer(unsigned bytes)
{
    int map = map_create(BPF_MAP_TYPE_RINGBUF, bytes);
    CHECK(map >= 0);
    volatile uint64_t *consumer = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, map, 0);
    unsigned char *mapping = mmap(NULL, PAGE + 2 * bytes, PROT_READ, MAP_SHARED, map, PAGE);
    CHECK(consumer != MAP_FAILED && mapping != MAP_FAILED);
    volatile uint64_t *position = (void *)mapping;
    unsigned char *data = mapping + PAGE;
    CHECK(*consumer == 0 && *position == 0);
    errno = 0;
    CHECK(mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, map, PAGE) == MAP_FAILED && errno == EPERM);
    errno = 0;
    CHECK(mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, map, 0) == MAP_FAILED && errno == EPERM);
    rejected_write(mapping, PAGE + 2 * bytes);
    int discard = producer(map, 1), submit = producer(map, 0);
    unsigned count = bytes / RECORD;
    CHECK(run(discard, count) == 0 && *position == (uint64_t)count * RECORD);
    CHECK(*(uint32_t *)data == (PAYLOAD | DISCARD));
    unsigned long offset = *position;
    CHECK(*(uint32_t *)(data + offset - RECORD) == (PAYLOAD | DISCARD));
    *consumer = offset;
    CHECK(run(submit, 1) == 0 && *position == offset + RECORD);
    CHECK(*(uint32_t *)(data + offset) == PAYLOAD);
    for (unsigned i = 0; i < PAYLOAD / 4; i++)
        CHECK(*(uint32_t *)(data + offset + 8 + i * 4) == 0x12345600 + i);
    CHECK(offset + RECORD > bytes && memcmp(data, data + bytes, bytes) == 0);
    struct pollfd pollfd = {.fd = map, .events = POLLIN};
    CHECK(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLIN));
    *consumer = *position;
    CHECK(run(submit, bytes / RECORD + 1) == 1);
    CHECK(*position - *consumer <= bytes - 1);
    CHECK(close(discard) == 0 && close(submit) == 0);
    CHECK(munmap((void *)consumer, PAGE) == 0 && munmap(mapping, PAGE + 2 * bytes) == 0 && close(map) == 0);
    printf("{\"test\":\"bpf_ringbuf_kernel_producer\",\"passed\":true,\"bytes\":%u,\"cross_tail_start\":%lu,\"discard_records\":%u}\n", bytes, offset, count);
}

static void user_producer(unsigned bytes)
{
    int map = map_create(BPF_MAP_TYPE_USER_RINGBUF, bytes);
    CHECK(map >= 0);
    volatile uint64_t *consumer = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, map, 0);
    unsigned char *mapping = mmap(NULL, PAGE + 2 * bytes, PROT_READ | PROT_WRITE, MAP_SHARED, map, PAGE);
    CHECK(consumer != MAP_FAILED && mapping != MAP_FAILED);
    volatile uint64_t *position = (void *)mapping;
    unsigned char *data = mapping + PAGE;
    errno = 0;
    CHECK(mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, map, 0) == MAP_FAILED && errno == EPERM);
    rejected_write((void *)consumer, PAGE);
    unsigned long offset = bytes - 8;
    *(uint32_t *)(data + offset) = PAYLOAD | BUSY;
    *(uint32_t *)(data + offset + 4) = 0;
    for (unsigned i = 0; i < PAYLOAD / 4; i++)
        *(uint32_t *)(data + offset + 8 + i * 4) = 0x56781200 + i;
    CHECK(memcmp(data, data + bytes, bytes) == 0);
    for (unsigned i = 0; i < PAYLOAD / 4; i++)
        CHECK(*(uint32_t *)(data + i * 4) == 0x56781200 + i);
    *position = RECORD;
    CHECK(*position == RECORD && *consumer == 0);
    CHECK(munmap((void *)consumer, PAGE) == 0 && munmap(mapping, PAGE + 2 * bytes) == 0 && close(map) == 0);
    printf("{\"test\":\"bpf_ringbuf_user_producer_mappings\",\"passed\":true,\"bytes\":%u,\"cross_tail_start\":%lu}\n", bytes, offset);
}

static long memory(const char *key)
{
    FILE *file = fopen("/proc/meminfo", "r");
    CHECK(file);
    char line[256];
    long value = -1;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, key, strlen(key))) {
            CHECK(sscanf(line + strlen(key), "%ld", &value) == 1);
            break;
        }
    }
    CHECK(fclose(file) == 0 && value >= 0);
    return value;
}

int main(void)
{
    setbuf(stdout, NULL);
    CHECK(sysconf(_SC_PAGESIZE) == PAGE);
    struct rlimit limit = {1UL << 30, 1UL << 30};
    CHECK(setrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    int control = map_create(BPF_MAP_TYPE_ARRAY, 2);
    CHECK(control >= 0 && close(control) == 0);
    for (unsigned type = BPF_MAP_TYPE_RINGBUF; type <= BPF_MAP_TYPE_USER_RINGBUF; type += 4) {
        for (unsigned bytes = PAGE; bytes < 16384; bytes *= 2) {
            errno = 0;
            CHECK(map_create(type, bytes) == -1 && errno == ENOMEM);
        }
    }
    for (unsigned round = 0; round < 4; round++) {
        long before = memory("VmallocUsed:");
        for (unsigned i = 0; i < 3; i++) {
            unsigned sizes[] = {16384, 32768, 4194304};
            kernel_producer(sizes[i]);
            user_producer(sizes[i]);
        }
        CHECK(usleep(10000) == 0);
        long after = memory("VmallocUsed:");
        printf("{\"test\":\"bpf_ringbuf_cleanup\",\"passed\":true,\"round\":%u,\"vmalloc_before_kib\":%ld,\"vmalloc_after_kib\":%ld}\n", round, before, after);
        CHECK(round == 0 || after <= before);
    }
    control = map_create(BPF_MAP_TYPE_ARRAY, 2);
    CHECK(control >= 0 && close(control) == 0);
    return 0;
}
