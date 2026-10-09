/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "seccomp-tests:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(111); \
        } \
    } while (0)
static volatile sig_atomic_t trapped;
static void trap_handler(int signal, siginfo_t *info, void *context)
{
    ucontext_t *state = context;
    CHECK(signal == SIGSYS && info->si_code == SYS_SECCOMP);
    CHECK(info->si_arch == AUDIT_ARCH_AARCH64 && info->si_syscall == SYS_getppid);
    CHECK((unsigned long)info->si_call_addr == state->uc_mcontext.pc);
    state->uc_mcontext.regs[0] = 731;
    ++trapped;
}

static void filter(unsigned action, unsigned syscall_number, unsigned flags)
{
    struct sock_filter rules[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, syscall_number, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, action),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {sizeof(rules) / sizeof(*rules), rules};
    CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    CHECK(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, flags, &program) == 0);
    CHECK(prctl(PR_GET_SECCOMP) == SECCOMP_MODE_FILTER);
}

static void wait_success(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
}

static void denied(void)
{
    errno = 0;
    CHECK(syscall(SYS_getppid) == -1 && errno == EACCES);
    CHECK(syscall(SYS_getpid) > 0);
}

struct thread_request {
    int read_fd, write_fd;
};
static void *synchronized_thread(void *opaque)
{
    struct thread_request *request = opaque;
    CHECK(write(request->write_fd, "r", 1) == 1);
    char ready;
    CHECK(read(request->read_fd, &ready, 1) == 1);
    denied();
    return NULL;
}

void seccomp_tests(void)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        filter(SECCOMP_RET_ERRNO | EACCES, SYS_getppid, 0);
        denied();
        pid_t inherited = fork();
        CHECK(inherited >= 0);
        if (!inherited) {
            denied();
            _exit(0);
        }
        wait_success(inherited);
        filter(SECCOMP_RET_ERRNO | E2BIG, 10000, 0);
        CHECK(syscall(10000) == -1 && errno == E2BIG);
        filter(SECCOMP_RET_ERRNO | EPERM, 243, 0);
        CHECK(syscall(243, 0, 0, 0, 0, 0, 0) == -1 && errno == EPERM);
        _exit(0);
    }
    wait_success(child);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        struct sigaction action = {.sa_sigaction = trap_handler, .sa_flags = SA_SIGINFO};
        CHECK(sigaction(SIGSYS, &action, NULL) == 0);
        filter(SECCOMP_RET_TRAP, SYS_getppid, 0);
        CHECK(syscall(SYS_getppid) == 731 && trapped == 1);
        _exit(0);
    }
    wait_success(child);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        filter(SECCOMP_RET_KILL_PROCESS, SYS_getppid, 0);
        syscall(SYS_getppid);
        _exit(112);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        int ready[2], proceed[2];
        CHECK(pipe(ready) == 0 && pipe(proceed) == 0);
        struct thread_request request = {proceed[0], ready[1]};
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, synchronized_thread, &request) == 0);
        char byte;
        CHECK(read(ready[0], &byte, 1) == 1);
        filter(SECCOMP_RET_ERRNO | EACCES, SYS_getppid, SECCOMP_FILTER_FLAG_TSYNC);
        CHECK(write(proceed[1], "r", 1) == 1 && pthread_join(thread, NULL) == 0);
        denied();
        _exit(0);
    }
    wait_success(child);
    puts("{\"test\":\"linux_seccomp_filter\",\"passed\":true,\"fork_inheritance\":true,\"thread_sync\":true,\"trap_metadata\":true}");
}
