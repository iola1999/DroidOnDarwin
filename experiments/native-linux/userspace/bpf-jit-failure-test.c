/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "BPF failure check line=%d errno=%d\n", __LINE__, errno); \
            exit(93); \
        } \
    } while (0)
#define I(code_, dst_, src_, off_, imm_) ((struct bpf_insn){.code = (code_), .dst_reg = (dst_), .src_reg = (src_), .off = (off_), .imm = (imm_)})
#define MOVI(reg, value) I(BPF_ALU64 | BPF_MOV | BPF_K, reg, 0, 0, value)
#define EXIT I(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)

static int bpf(unsigned command, union bpf_attr *attr)
{
    return syscall(SYS_bpf, command, attr, sizeof(*attr));
}

int main(void)
{
    setbuf(stdout, NULL);
    struct rlimit limit = {1024UL * 1024 * 1024, 1024UL * 1024 * 1024};
    CHECK(setrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    int sysctl = open("/proc/sys/net/core/bpf_jit_enable", O_WRONLY | O_CLOEXEC);
    CHECK(sysctl >= 0 && write(sysctl, "1\n", 2) == 2 && close(sysctl) == 0);
    struct bpf_insn single[] = {MOVI(0, 42), EXIT};
    struct bpf_insn multiple[] = {
        MOVI(1, 41),
        I(BPF_JMP | BPF_CALL, 0, BPF_PSEUDO_CALL, 0, 1),
        EXIT,
        I(BPF_ALU64 | BPF_MOV | BPF_X, 0, 1, 0, 0),
        I(BPF_ALU64 | BPF_ADD | BPF_K, 0, 0, 0, 1),
        EXIT,
    };
    struct bpf_insn *insns = SUBPROGRAM ? multiple : single;
    unsigned count = SUBPROGRAM ? sizeof(multiple) / sizeof(*multiple) : sizeof(single) / sizeof(*single);
    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        char verifier[65536] = {0};
        union bpf_attr load = {.prog_type = BPF_PROG_TYPE_SOCKET_FILTER, .insn_cnt = count, .insns = (uintptr_t)insns, .license = (uintptr_t)"GPL", .log_buf = (uintptr_t)verifier, .log_size = sizeof(verifier), .log_level = 1};
        int fd = bpf(BPF_PROG_LOAD, &load);
        if (fd < 0)
            fprintf(stderr, "load: %s\n", verifier);
        CHECK(fd >= 0);
        struct bpf_prog_info info = {0};
        union bpf_attr query = {0};
        query.info.bpf_fd = fd;
        query.info.info_len = sizeof(info);
        query.info.info = (uintptr_t)&info;
        CHECK(bpf(BPF_OBJ_GET_INFO_BY_FD, &query) == 0 && info.xlated_prog_len > 0);
        CHECK(FAIL_PUBLICATION ? info.jited_prog_len == 0 : info.jited_prog_len > 0);
        printf("{\"test\":\"bpf_failure_loaded\",\"iteration\":%u,\"subprogram\":%s,\"jited_prog_len\":%u}\n",
               iteration, SUBPROGRAM ? "true" : "false", info.jited_prog_len);
        unsigned char packet[64] = {0};
        union bpf_attr run = {0};
        run.test.prog_fd = fd;
        run.test.data_in = (uintptr_t)packet;
        run.test.data_size_in = sizeof(packet);
        run.test.repeat = 1;
        CHECK(bpf(BPF_PROG_TEST_RUN, &run) == 0 && run.test.retval == 42);
        CHECK(close(fd) == 0);
        CHECK(usleep(300000) == 0);
        printf("{\"test\":\"bpf_failure_executed\",\"iteration\":%u,\"retval\":42,\"closed\":true}\n", iteration);
    }
    return 0;
}
