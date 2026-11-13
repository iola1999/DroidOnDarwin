/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "BPF JIT check line=%d errno=%d\n", __LINE__, errno); \
            exit(90); \
        } \
    } while (0)
#define I(code_, dst_, src_, off_, imm_) ((struct bpf_insn){.code = (code_), .dst_reg = (dst_), .src_reg = (src_), .off = (off_), .imm = (imm_)})
#define MOVI(reg, value) I(BPF_ALU64 | BPF_MOV | BPF_K, reg, 0, 0, value)
#define MOVR(dst, src) I(BPF_ALU64 | BPF_MOV | BPF_X, dst, src, 0, 0)
#define EXIT I(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

static int bpf(unsigned command, union bpf_attr *attr)
{
    return syscall(SYS_bpf, command, attr, sizeof(*attr));
}

static void sysctl_one(const char *path)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    CHECK(fd >= 0 && write(fd, "1\n", 2) == 2 && close(fd) == 0);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    char value[8] = {0};
    CHECK(fd >= 0 && read(fd, value, sizeof(value) - 1) == 2 && close(fd) == 0);
    CHECK(!strcmp(value, "1\n"));
}

static void program(const char *name, struct bpf_insn *instructions, unsigned count, unsigned expected)
{
    char verifier[65536] = {0};
    union bpf_attr load = {.prog_type = BPF_PROG_TYPE_SOCKET_FILTER,
                           .insn_cnt = count,
                           .insns = (uintptr_t)instructions,
                           .license = (uintptr_t)"GPL",
                           .log_buf = (uintptr_t)verifier,
                           .log_size = sizeof(verifier),
                           .log_level = 1};
    int fd = bpf(BPF_PROG_LOAD, &load);
    if (fd < 0)
        fprintf(stderr, "%s load errno=%d: %s\n", name, errno, verifier);
    CHECK(fd >= 0);
    struct bpf_prog_info info = {0};
    union bpf_attr query = {0};
    query.info.bpf_fd = fd;
    query.info.info_len = sizeof(info);
    query.info.info = (uintptr_t)&info;
    CHECK(bpf(BPF_OBJ_GET_INFO_BY_FD, &query) == 0);
    CHECK(info.type == BPF_PROG_TYPE_SOCKET_FILTER && info.jited_prog_len > 0 && info.xlated_prog_len > 0);
    unsigned char packet[64] = {0};
    union bpf_attr run = {0};
    run.test.prog_fd = fd;
    run.test.data_in = (uintptr_t)packet;
    run.test.data_size_in = sizeof(packet);
    run.test.repeat = 1;
    CHECK(bpf(BPF_PROG_TEST_RUN, &run) == 0 && run.test.retval == expected);
    CHECK(close(fd) == 0);
    printf("{\"test\":\"%s\",\"passed\":true,\"retval\":%u,\"jited_prog_len\":%u,\"xlated_prog_len\":%u}\n",
           name, run.test.retval, info.jited_prog_len, info.xlated_prog_len);
}

struct array_value {
    uint64_t wide;
    uint32_t narrow;
    uint32_t padding;
};

static void array_value(int map, uint64_t wide, uint32_t narrow)
{
    uint32_t key = 0;
    struct array_value value = {0};
    union bpf_attr attr = {.map_fd = map, .key = (uintptr_t)&key, .value = (uintptr_t)&value};
    CHECK(bpf(BPF_MAP_LOOKUP_ELEM, &attr) == 0 && value.wide == wide && value.narrow == narrow && value.padding == 0);
}

static unsigned lookup(struct bpf_insn *instructions, int map)
{
    instructions[0] = I(BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, map);
    instructions[1] = I(0, 0, 0, 0, 0);
    instructions[2] = MOVR(2, 10);
    instructions[3] = I(BPF_ALU64 | BPF_ADD | BPF_K, 2, 0, 0, -4);
    instructions[4] = I(BPF_ST | BPF_MEM | BPF_W, 10, 0, -4, 0);
    instructions[5] = I(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_map_lookup_elem);
    instructions[6] = I(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 0, 0);
    return 7;
}

static unsigned lookup_end(struct bpf_insn *instructions, unsigned count)
{
    instructions[count++] = EXIT;
    instructions[6].off = count - 7;
    instructions[count++] = MOVI(0, 0);
    instructions[count++] = EXIT;
    return count;
}

static void array_tests(void)
{
    union bpf_attr create = {.map_type = BPF_MAP_TYPE_ARRAY, .key_size = 4, .value_size = sizeof(struct array_value), .max_entries = 1};
    int map = bpf(BPF_MAP_CREATE, &create);
    CHECK(map >= 0);
    uint32_t key = 0;
    struct array_value value = {.wide = 100, .narrow = 7};
    union bpf_attr update = {.map_fd = map, .key = (uintptr_t)&key, .value = (uintptr_t)&value};
    CHECK(bpf(BPF_MAP_UPDATE_ELEM, &update) == 0);
    struct bpf_insn instructions[32];
    unsigned count = lookup(instructions, map);
    instructions[count++] = I(BPF_LDX | BPF_MEM | BPF_DW, 0, 0, 0, 0);
    count = lookup_end(instructions, count);
    program("bpf_jit_array_lookup", instructions, count, 100);
    array_value(map, 100, 7);

    count = lookup(instructions, map);
    instructions[count++] = MOVR(6, 0);
    instructions[count++] = MOVI(1, 5);
    instructions[count++] = I(BPF_STX | BPF_ATOMIC | BPF_DW, 6, 1, 0, BPF_ADD | BPF_FETCH);
    instructions[count++] = MOVR(0, 1);
    count = lookup_end(instructions, count);
    program("bpf_jit_atomic64_fetch", instructions, count, 100);
    array_value(map, 105, 7);

    count = lookup(instructions, map);
    instructions[count++] = MOVR(6, 0);
    instructions[count++] = MOVI(0, 105);
    instructions[count++] = MOVI(1, 211);
    instructions[count++] = I(BPF_STX | BPF_ATOMIC | BPF_DW, 6, 1, 0, BPF_CMPXCHG);
    count = lookup_end(instructions, count);
    program("bpf_jit_atomic64_cmpxchg", instructions, count, 105);
    array_value(map, 211, 7);

    count = lookup(instructions, map);
    instructions[count++] = MOVR(6, 0);
    instructions[count++] = MOVI(1, 19);
    instructions[count++] = I(BPF_STX | BPF_ATOMIC | BPF_W, 6, 1, 8, BPF_XCHG);
    instructions[count++] = MOVR(0, 1);
    count = lookup_end(instructions, count);
    program("bpf_jit_atomic32_xchg", instructions, count, 7);
    array_value(map, 211, 19);

    count = lookup(instructions, map);
    instructions[count++] = MOVR(6, 0);
    instructions[count++] = MOVI(0, 19);
    instructions[count++] = MOVI(1, 23);
    instructions[count++] = I(BPF_STX | BPF_ATOMIC | BPF_W, 6, 1, 8, BPF_CMPXCHG);
    count = lookup_end(instructions, count);
    program("bpf_jit_atomic32_cmpxchg", instructions, count, 19);
    array_value(map, 211, 23);
    CHECK(close(map) == 0);
}

static void ringbuf_test(void)
{
    unsigned page = sysconf(_SC_PAGESIZE);
    CHECK(page == 4096 || page == 16384);
    unsigned bytes = 4 * page;
    union bpf_attr create = {.map_type = BPF_MAP_TYPE_RINGBUF, .max_entries = bytes};
    int map = bpf(BPF_MAP_CREATE, &create);
    CHECK(map >= 0);
    volatile uint64_t *consumer = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, map, 0);
    unsigned char *producer = mmap(NULL, page + 2 * bytes, PROT_READ, MAP_SHARED, map, page);
    CHECK(consumer != MAP_FAILED && producer != MAP_FAILED);
    volatile uint64_t *position = (void *)producer;
    unsigned char *data = producer + page;
    CHECK(*consumer == 0 && *position == 0);
    errno = 0;
    CHECK(mprotect(producer, page + 2 * bytes, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    struct bpf_insn instructions[] = {
        I(BPF_LD | BPF_DW | BPF_IMM, 1, BPF_PSEUDO_MAP_FD, 0, map),
        I(0, 0, 0, 0, 0),
        MOVI(2, 8),
        MOVI(3, 0),
        I(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_ringbuf_reserve),
        I(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 6, 0),
        I(BPF_ST | BPF_MEM | BPF_DW, 0, 0, 0, 0x12345678),
        MOVR(1, 0),
        MOVI(2, 0),
        I(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_ringbuf_submit),
        MOVI(0, 0),
        EXIT,
        MOVI(0, 1),
        EXIT,
    };
    program("bpf_jit_ringbuf_reserve_submit", instructions, sizeof(instructions) / sizeof(*instructions), 0);
    CHECK(*consumer == 0 && *position == 16 && *(uint32_t *)data == 8 && *(uint64_t *)(data + 8) == 0x12345678);
    *consumer = *position;
    CHECK(munmap((void *)consumer, page) == 0 && munmap(producer, page + 2 * bytes) == 0 && close(map) == 0);
    printf("{\"test\":\"bpf_jit_ringbuf_record\",\"passed\":true,\"page_bytes\":%u,\"bytes\":%u,\"record_bytes\":16}\n", page, bytes);
}

int main(void)
{
    CHECK(mkdir("/proc", 0755) == 0 || errno == EEXIST);
    CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0 || errno == EBUSY);
    struct rlimit limit = {.rlim_cur = 1024UL * 1024 * 1024, .rlim_max = 1024UL * 1024 * 1024};
    CHECK(setrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    sysctl_one("/proc/sys/net/core/bpf_jit_enable");
    sysctl_one("/proc/sys/net/core/bpf_jit_kallsyms");
    puts("{\"test\":\"bpf_jit_sysctls\",\"passed\":true,\"bpf_jit_enable\":1,\"bpf_jit_kallsyms\":1}");

    struct bpf_insn arithmetic[] = {
        MOVI(0, 73),
        I(BPF_ALU64 | BPF_MUL | BPF_K, 0, 0, 0, 3),
        I(BPF_ALU64 | BPF_DIV | BPF_K, 0, 0, 0, 7),
        I(BPF_ALU64 | BPF_MOD | BPF_K, 0, 0, 0, 13),
        I(BPF_ALU64 | BPF_LSH | BPF_K, 0, 0, 0, 33),
        I(BPF_ALU | BPF_ADD | BPF_K, 0, 0, 0, 9),
        MOVI(2, 5),
        I(BPF_ALU64 | BPF_ADD | BPF_X, 0, 2, 0, 0),
        EXIT,
    };
    program("bpf_jit_arithmetic", arithmetic, sizeof(arithmetic) / sizeof(*arithmetic), 14);
    struct bpf_insn helper[] = {
        I(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_get_smp_processor_id),
        I(BPF_ALU64 | BPF_ADD | BPF_K, 0, 0, 0, 41),
        EXIT,
    };
    program("bpf_jit_helper", helper, sizeof(helper) / sizeof(*helper), 41);
    array_tests();
    struct bpf_insn subprogram[] = {
        MOVI(1, 41),
        I(BPF_JMP | BPF_CALL, 0, BPF_PSEUDO_CALL, 0, 1),
        EXIT,
        MOVR(0, 1),
        I(BPF_ALU64 | BPF_ADD | BPF_K, 0, 0, 0, 1),
        EXIT,
    };
    program("bpf_jit_subprogram", subprogram, sizeof(subprogram) / sizeof(*subprogram), 42);
    ringbuf_test();
    puts("{\"test\":\"bpf_jit_guest_cleanup\",\"passed\":true}");
    return 0;
}
