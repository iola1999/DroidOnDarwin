/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct arm64_registers {
    uint64_t x[31], sp, pc, pstate;
};

struct shared {
    uint64_t counter;
    unsigned returned, clone_entered;
    struct arm64_registers snapshot;
    uint64_t tls_before, tls_after, tls_requested;
    uint64_t clone_sp_before, clone_sp_after, clone_fp_before, clone_fp_after;
    uint64_t clone_x18_before, clone_x18_after, clone_nzcv_before, clone_nzcv_after;
    unsigned snapshot_ready, clone_release;
};

_Static_assert(sizeof(struct arm64_registers) == 272, "ARM64 NT_PRSTATUS layout");
_Static_assert(offsetof(struct shared, snapshot) == 16, "assembly snapshot offset");
_Static_assert(offsetof(struct shared, tls_before) == 288, "assembly TLS offset");
_Static_assert(offsetof(struct shared, clone_sp_before) == 312, "assembly clone SP offset");
_Static_assert(offsetof(struct shared, clone_fp_before) == 328, "assembly clone FP offset");
_Static_assert(offsetof(struct shared, clone_x18_before) == 344, "assembly clone x18 offset");
_Static_assert(offsetof(struct shared, clone_nzcv_before) == 360, "assembly clone NZCV offset");
_Static_assert(offsetof(struct shared, snapshot_ready) == 376, "assembly ready offset");
_Static_assert(offsetof(struct shared, clone_release) == 380, "assembly release offset");

/* 汇编快照保留全部 GPR；循环仅改变 x17，按停止 PC 与已写入 counter 核对。 */
#define SEED_STABLE_REGISTERS \
    "mov x3, #0x1003\n" \
    "mov x4, #0x1004\n" \
    "mov x5, #0x1005\n" \
    "mov x6, #0x1006\n" \
    "mov x7, #0x1007\n" \
    "mov x9, #0x1009\n" \
    "mov x10, #0x100a\n" \
    "mov x11, #0x100b\n" \
    "mov x12, #0x100c\n" \
    "mov x13, #0x100d\n" \
    "mov x14, #0x100e\n" \
    "mov x15, #0x100f\n" \
    "mov x18, #0x1012\n" \
    "mov x19, #0x1013\n" \
    "mov x20, #0x1014\n" \
    "mov x21, #0x1015\n" \
    "mov x22, #0x1016\n" \
    "mov x23, #0x1017\n" \
    "mov x24, #0x1018\n" \
    "mov x25, #0x1019\n" \
    "mov x26, #0x101a\n" \
    "mov x27, #0x101b\n" \
    "mov x28, #0x101c\n" \
    "mov x29, #0x101d\n" \
    "mov x30, #0x101e\n"

#define STORE_SNAPSHOT(BASE, PC) \
    "stp x0, x1, [" BASE \
    ", #16]\n" \
    "stp x2, x3, [" BASE \
    ", #32]\n" \
    "stp x4, x5, [" BASE \
    ", #48]\n" \
    "stp x6, x7, [" BASE \
    ", #64]\n" \
    "stp x8, x9, [" BASE \
    ", #80]\n" \
    "stp x10, x11, [" BASE \
    ", #96]\n" \
    "stp x12, x13, [" BASE \
    ", #112]\n" \
    "stp x14, x15, [" BASE \
    ", #128]\n" \
    "stp x16, x17, [" BASE \
    ", #144]\n" \
    "stp x18, x19, [" BASE \
    ", #160]\n" \
    "stp x20, x21, [" BASE \
    ", #176]\n" \
    "stp x22, x23, [" BASE \
    ", #192]\n" \
    "stp x24, x25, [" BASE \
    ", #208]\n" \
    "stp x26, x27, [" BASE \
    ", #224]\n" \
    "stp x28, x29, [" BASE \
    ", #240]\n" \
    "str x30, [" BASE \
    ", #256]\n" \
    "mov x9, sp\n" \
    "str x9, [" BASE \
    ", #264]\n" \
    "adr x9, " PC \
    "\n" \
    "str x9, [" BASE \
    ", #272]\n" \
    "mrs x9, nzcv\n" \
    "str x9, [" BASE \
    ", #280]\n" \
    "add x9, " BASE \
    ", #376\n" \
    "mov w10, #1\n" \
    "stlr w10, [x9]\n" \
    "mov x9, #0x1009\n" \
    "mov x10, #0x100a\n"

extern void ptrace_user_loop(struct shared *) __attribute__((noreturn));
extern long ptrace_blocking_read(struct shared *, int, unsigned char *);
extern long ptrace_clone_settls(struct shared *, uintptr_t);
extern const unsigned char ptrace_loop_start[], ptrace_read_svc[];
extern const unsigned char ptrace_clone_after_svc[], ptrace_clone_child_loop[];

__asm__(
    ".text\n"
    ".p2align 2\n"
    ".global ptrace_user_loop\n"
    ".type ptrace_user_loop, %function\n"
    "ptrace_user_loop:\n"
    "sub sp, sp, #8\n" SEED_STABLE_REGISTERS
    "mov x1, #0x1001\n"
    "mov x2, #0x1002\n"
    "mov x8, #0x1008\n"
    "mov x16, x0\n"
    "mov x17, #0\n"
    "mov x9, #0xa0000000\n"
    "msr nzcv, x9\n"
    "mov x9, #0x1009\n" STORE_SNAPSHOT("x0", "ptrace_loop_start")
        ".global ptrace_loop_start\n"
        "ptrace_loop_start:\n"
        "add x17, x17, #1\n"
        "str x17, [x16]\n"
        "b ptrace_loop_start\n"
        ".size ptrace_user_loop, .-ptrace_user_loop\n"
        ".p2align 2\n"
        ".global ptrace_blocking_read\n"
        ".type ptrace_blocking_read, %function\n"
        "ptrace_blocking_read:\n"
        "stp x18, x19, [sp, #-112]!\n"
        "stp x20, x21, [sp, #16]\n"
        "stp x22, x23, [sp, #32]\n"
        "stp x24, x25, [sp, #48]\n"
        "stp x26, x27, [sp, #64]\n"
        "stp x28, x29, [sp, #80]\n"
        "str x30, [sp, #96]\n"
        "sub sp, sp, #8\n"
        "mov x16, x0\n"
        "mov x0, x1\n"
        "mov x1, x2\n"
        "mov x2, #1\n"
        "mov x8, #63\n"
        "mov x17, #0x1011\n" SEED_STABLE_REGISTERS
        "mov x9, #0xa0000000\n"
        "msr nzcv, x9\n"
        "mov x9, #0x1009\n" STORE_SNAPSHOT("x16", "ptrace_read_svc")
            ".global ptrace_read_svc\n"
            "ptrace_read_svc:\n"
            "svc #0\n"
            "add sp, sp, #8\n"
            "ldp x20, x21, [sp, #16]\n"
            "ldp x22, x23, [sp, #32]\n"
            "ldp x24, x25, [sp, #48]\n"
            "ldp x26, x27, [sp, #64]\n"
            "ldp x28, x29, [sp, #80]\n"
            "ldr x30, [sp, #96]\n"
            "ldp x18, x19, [sp], #112\n"
            "ret\n"
            ".size ptrace_blocking_read, .-ptrace_blocking_read\n"
            ".p2align 2\n"
            ".global ptrace_clone_settls\n"
            ".type ptrace_clone_settls, %function\n"
            "ptrace_clone_settls:\n"
            "stp x18, x19, [sp, #-48]!\n"
            "stp x20, x21, [sp, #16]\n"
            "stp x29, x30, [sp, #32]\n"
            "sub sp, sp, #8\n"
            "mov x19, x0\n"
            "mov x21, x1\n"
            "mrs x20, tpidr_el0\n"
            "mov x9, sp\n"
            "str x9, [x19, #312]\n"
            "str x29, [x19, #328]\n"
            "mov x18, #0x5218\n"
            "str x18, [x19, #344]\n"
            "mov x9, #0xa0000000\n"
            "msr nzcv, x9\n"
            "str x9, [x19, #360]\n"
            "mov x0, #0x400\n"
            "movk x0, #8, lsl #16\n"
            "mov x1, xzr\n"
            "mov x2, xzr\n"
            "mov x3, x21\n"
            "mov x4, xzr\n"
            "mov x8, #220\n"
            "svc #0\n"
            ".global ptrace_clone_after_svc\n"
            "ptrace_clone_after_svc:\n"
            "cbnz x0, 2f\n"
            "mrs x9, tpidr_el0\n"
            "str x9, [x19, #296]\n"
            "mov x9, sp\n"
            "str x9, [x19, #320]\n"
            "str x29, [x19, #336]\n"
            "str x18, [x19, #352]\n"
            "mrs x9, nzcv\n"
            "str x9, [x19, #368]\n"
            "add x9, x19, #12\n"
            "mov w10, #1\n"
            "stlr w10, [x9]\n"
            ".global ptrace_clone_child_loop\n"
            "ptrace_clone_child_loop:\n"
            "ldr w9, [x19, #380]\n"
            "cbz w9, ptrace_clone_child_loop\n"
            "2:\n"
            /* 客体 TLS 恢复后才返回 C，后续 libc 调用使用原线程指针。 */
            "msr tpidr_el0, x20\n"
            "add sp, sp, #8\n"
            "ldp x20, x21, [sp, #16]\n"
            "ldp x29, x30, [sp, #32]\n"
            "ldp x18, x19, [sp], #48\n"
            "ret\n"
            ".size ptrace_clone_settls, .-ptrace_clone_settls\n");

#undef SEED_STABLE_REGISTERS
#undef STORE_SNAPSHOT

static int regset_supported = -1;

struct fixture {
    const char *name;
    pid_t leader;
    int gate;
    struct shared *shared;
};

static uint64_t now(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        exit(2);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void pause_ms(unsigned milliseconds)
{
    struct timespec value = {0, (long)milliseconds * 1000000};
    while (nanosleep(&value, &value) && errno == EINTR) {
    }
}

/* 父子进程共享 stdout，每条记录须在同一次 write 中输出。 */
static void emit_json(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void emit_json(const char *format, ...)
{
    char line[4096];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(line))
        _exit(2);
    ssize_t written;
    do {
        written = write(STDOUT_FILENO, line, (size_t)length);
    } while (written < 0 && errno == EINTR);
    if (written != length)
        _exit(2);
}

static int check(const char *name, const char *step, int passed, long observed)
{
    emit_json("{\"test\":\"lkl_ptrace\",\"case\":\"%s\",\"step\":\"%s\",\"observed\":%ld,\"passed\":%s}\n",
              name, step, observed, passed ? "true" : "false");
    return passed;
}

static int request(const char *name, const char *step, int operation, pid_t target,
                   void *address, void *data, int expected_errno)
{
    errno = 0;
    long result = ptrace(operation, target, address, data);
    int error = errno;
    int passed = expected_errno ? result == -1 && error == expected_errno : result == 0;
    emit_json(
        "{\"test\":\"lkl_ptrace\",\"case\":\"%s\",\"step\":\"%s\",\"request\":%d,\"target\":%d,"
        "\"result\":%ld,\"errno\":%d,\"expected_errno\":%d,\"passed\":%s}\n",
        name, step, operation, target, result, error, expected_errno, passed ? "true" : "false");
    return passed;
}

static pid_t wait_for(pid_t target, int *status, int options)
{
    pid_t result;
    do {
        result = waitpid(target, status, options);
    } while (result < 0 && errno == EINTR);
    return result;
}

static int stopped(const char *name, const char *step, pid_t target, int event)
{
    int status = 0;
    pid_t result = wait_for(target, &status, __WALL);
    return check(name, step, result == target && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP && status >> 16 == event,
                 result == target ? status : -errno);
}

static int wait_running(struct fixture *fixture, uint64_t previous)
{
    uint64_t deadline = now() + 5000000000;
    while (__atomic_load_n(&fixture->shared->counter, __ATOMIC_ACQUIRE) == previous) {
        if (now() >= deadline)
            return 0;
        sched_yield();
    }
    return 1;
}

static int wait_sleeping(struct fixture *fixture)
{
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", fixture->leader);
    uint64_t deadline = now() + 5000000000;
    do {
        FILE *input = fopen(path, "r");
        if (!input)
            return 0;
        char *read = fgets(line, sizeof(line), input);
        fclose(input);
        char *end = read ? strrchr(line, ')') : NULL;
        if (end && end[1] == ' ' && end[2] == 'S')
            return 1;
        pause_ms(2);
    } while (now() < deadline);
    return 0;
}

static int cleanup(struct fixture *fixture)
{
    int passed = 1;
    if (fixture->leader > 0) {
        /* 子任务共用独立进程组，失败回收覆盖尚未取得 PID 的 clone 子任务。 */
        if (kill(-fixture->leader, SIGKILL) && errno != ESRCH)
            passed = 0;
        kill(fixture->leader, SIGKILL);
        for (;;) {
            int status;
            pid_t result = wait_for(-fixture->leader, &status, __WALL);
            if (result < 0) {
                passed &= errno == ECHILD;
                break;
            }
            if (WIFSTOPPED(status))
                passed &= kill(result, SIGKILL) == 0;
        }
    }
    if (fixture->gate >= 0)
        passed &= close(fixture->gate) == 0;
    if (fixture->shared != MAP_FAILED)
        passed &= munmap(fixture->shared, 16384) == 0;
    int status;
    passed &= wait_for(-1, &status, __WALL | WNOHANG) == -1 && errno == ECHILD;
    return check(fixture->name, "owned_children_reaped", passed, passed);
}

static int start(struct fixture *fixture, int mode)
{
    int gate[2], ready[2];
    fixture->leader = 0;
    fixture->gate = -1;
    fixture->shared = mmap(NULL, 16384, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (fixture->shared == MAP_FAILED || pipe(gate))
        return 0;
    fixture->shared->tls_requested = (uintptr_t)fixture->shared + 8192;
    if (pipe(ready)) {
        close(gate[0]);
        close(gate[1]);
        return 0;
    }
    pid_t child = fork();
    if (child < 0) {
        close(gate[0]);
        close(gate[1]);
        close(ready[0]);
        close(ready[1]);
        return 0;
    }
    if (!child) {
        close(gate[1]);
        close(ready[0]);
        if (setpgid(0, 0))
            _exit(120);
        uintptr_t initial_tls;
        __asm__ volatile("mrs %0, tpidr_el0" : "=r"(initial_tls));
        fixture->shared->tls_before = initial_tls;
        if (write(ready[1], "1", 1) != 1)
            _exit(121);
        close(ready[1]);
        unsigned char command = 0;
        errno = 0;
        ssize_t result = mode == 1 ? ptrace_blocking_read(fixture->shared, gate[0], &command) : read(gate[0], &command, 1);
        if (mode == 1)
            emit_json(
                "{\"test\":\"lkl_ptrace\",\"case\":\"blocking_read\",\"step\":\"user_read_return\","
                "\"result\":%ld,\"errno\":%d,\"passed\":%s}\n",
                (long)result, errno,
                result == 1 && command == 0x5a ? "true" : "false");
        if (result != 1 || command != 0x5a)
            _exit(122);
        if (mode == 0)
            ptrace_user_loop(fixture->shared);
        if (mode == 1) {
            uintptr_t restored_tls;
            __asm__ volatile("mrs %0, tpidr_el0" : "=r"(restored_tls));
            if (!check(fixture->name, "blocking_read_tls_restored", restored_tls == initial_tls, restored_tls))
                _exit(127);
            __atomic_store_n(&fixture->shared->returned, 1, __ATOMIC_RELEASE);
            _exit(42);
        }
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &blocked, NULL))
            _exit(126);
        uintptr_t tls_before, sp_before, fp_before;
        __asm__ volatile("mrs %0, tpidr_el0\nmov %1, sp\nmov %2, x29"
                         : "=r"(tls_before), "=r"(sp_before), "=r"(fp_before));
        pid_t clone_pid = mode == 3 ? ptrace_clone_settls(fixture->shared, fixture->shared->tls_requested) : syscall(SYS_clone, (unsigned long)CLONE_FILES, 0L, 0L, 0L, 0L);
        if (clone_pid < 0)
            _exit(123);
        if (!clone_pid) {
            if (mode == 2)
                __atomic_store_n(&fixture->shared->clone_entered, 1, __ATOMIC_RELEASE);
            uintptr_t tls_after, sp_after, fp_after;
            __asm__ volatile("mrs %0, tpidr_el0\nmov %1, sp\nmov %2, x29"
                             : "=r"(tls_after), "=r"(sp_after), "=r"(fp_after));
            int child_passed = tls_before == tls_after && sp_before == sp_after && fp_before == fp_after;
            syscall(SYS_getpid);
            child_passed = check(fixture->name, "clone_tls_stack_frame_preserved", child_passed, tls_after);
            if (mode == 3)
                child_passed &= check(fixture->name, "clone_requested_tls_and_raw_state_preserved",
                                      fixture->shared->tls_after == fixture->shared->tls_requested &&
                                          fixture->shared->clone_sp_before == fixture->shared->clone_sp_after &&
                                          fixture->shared->clone_fp_before == fixture->shared->clone_fp_after &&
                                          fixture->shared->clone_x18_before == fixture->shared->clone_x18_after &&
                                          fixture->shared->clone_nzcv_before == fixture->shared->clone_nzcv_after,
                                      fixture->shared->tls_after);
            sigset_t mask, pending;
            child_passed &= check(fixture->name, "clone_signal_mask_preserved",
                                  !sigprocmask(SIG_SETMASK, NULL, &mask) && sigismember(&mask, SIGUSR1) == 1, 0);
            child_passed &= check(fixture->name, "clone_blocked_signal_pending",
                                  !sigpending(&pending) && sigismember(&pending, SIGUSR1) == 1, 0);
            _exit(child_passed ? 43 : 125);
        }
        uintptr_t parent_tls;
        __asm__ volatile("mrs %0, tpidr_el0" : "=r"(parent_tls));
        if (!check(fixture->name, "clone_parent_tls_preserved", parent_tls == tls_before, parent_tls))
            _exit(128);
        int status = 0;
        if (wait_for(clone_pid, &status, __WCLONE) != clone_pid ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 43)
            _exit(124);
        _exit(44);
    }
    fixture->leader = child;
    close(gate[0]);
    close(ready[1]);
    fixture->gate = gate[1];
    char byte;
    int passed = read(ready[0], &byte, 1) == 1 && byte == '1';
    close(ready[0]);
    return passed;
}

static int filled(const void *memory, size_t bytes, unsigned char value)
{
    const unsigned char *data = memory;
    for (size_t index = 0; index < bytes; ++index)
        if (data[index] != value)
            return 0;
    return 1;
}

static int get_gpr(struct fixture *fixture, pid_t target, struct arm64_registers *registers)
{
    memset(registers, 0xa5, sizeof(*registers));
    struct iovec vector = {registers, sizeof(*registers)};
    errno = 0;
    long result = ptrace(PTRACE_GETREGSET, target, (void *)1, &vector);
    int error = errno;
    if (regset_supported < 0) {
        regset_supported = result == 0;
        emit_json("{\"test\":\"lkl_ptrace_regset_capability\",\"supported\":%s}\n",
                  regset_supported ? "true" : "false");
    }
    int expected_errno = regset_supported ? 0 : EIO;
    int passed = regset_supported ? result == 0 && vector.iov_len == sizeof(*registers) : result == -1 && error == EIO && vector.iov_len == sizeof(*registers) && filled(registers, sizeof(*registers), 0xa5);
    passed &= vector.iov_base == registers;
    emit_json(
        "{\"test\":\"lkl_ptrace\",\"case\":\"%s\",\"step\":\"getregset_prstatus\","
        "\"request\":%d,\"target\":%d,\"result\":%ld,\"errno\":%d,\"expected_errno\":%d,"
        "\"iov_len\":%zu,\"passed\":%s}\n",
        fixture->name, PTRACE_GETREGSET, target, result, error, expected_errno,
        vector.iov_len, passed ? "true" : "false");
    return passed;
}

static void format_register_words(char text[34 * 21], const uint64_t words[34])
{
    size_t used = 0;
    for (unsigned index = 0; index < 34; ++index) {
        int length = snprintf(text + used, 34 * 21 - used, "%s%llu",
                              index ? "," : "", (unsigned long long)words[index]);
        if (length < 0 || (size_t)length >= 34 * 21 - used)
            _exit(2);
        used += (size_t)length;
    }
}

static int gpr_snapshot(struct fixture *fixture, pid_t target,
                        const struct arm64_registers *registers, int mode)
{
    if (!regset_supported || !mode)
        return 1;
    uint64_t mask = (UINT64_C(1) << 34) - 1;
    if (mode <= 2 && !check(fixture->name, "assembly_snapshot_ready",
                            __atomic_load_n(&fixture->shared->snapshot_ready, __ATOMIC_ACQUIRE) == 1, 0))
        return 0;
    struct arm64_registers expected = mode <= 2 ? fixture->shared->snapshot : *registers;
    if (mode == 1) {
        uint64_t begin = (uintptr_t)ptrace_loop_start;
        if (!check(fixture->name, "stopped_pc_in_raw_user_loop",
                   registers->pc == begin || registers->pc == begin + 4 || registers->pc == begin + 8,
                   registers->pc))
            return 0;
        expected.pc = registers->pc;
        expected.x[17] = __atomic_load_n(&fixture->shared->counter, __ATOMIC_ACQUIRE) + (registers->pc == begin + 4);
    } else if (mode == 2) {
        /* nce_user_return 在 read 的信号停止前恢复原 x0，并将 PC 调整到 svc。 */
        expected.pc = (uintptr_t)ptrace_read_svc;
    } else {
        expected.x[0] = 0;
        expected.x[18] = fixture->shared->clone_x18_before;
        expected.x[29] = fixture->shared->clone_fp_before;
        expected.sp = fixture->shared->clone_sp_before;
        expected.pstate = fixture->shared->clone_nzcv_before;
        expected.pc = (uintptr_t)(mode == 3 ? ptrace_clone_after_svc : ptrace_clone_child_loop);
        if (mode == 4 && registers->pc == expected.pc + 4)
            expected.pc += 4;
        mask = (UINT64_C(1) << 0) | (UINT64_C(1) << 18) | (UINT64_C(1) << 29) |
               (UINT64_C(1) << 31) | (UINT64_C(1) << 32) | (UINT64_C(1) << 33);
    }
    uint64_t differences = 0;
    uint64_t actual_words[34], expected_words[34];
    memcpy(actual_words, registers, sizeof(actual_words));
    memcpy(expected_words, &expected, sizeof(expected_words));
    for (unsigned index = 0; index < 34; ++index)
        if ((mask & (UINT64_C(1) << index)) && actual_words[index] != expected_words[index])
            differences |= UINT64_C(1) << index;
    char actual_text[34 * 21], expected_text[34 * 21];
    format_register_words(actual_text, actual_words);
    format_register_words(expected_text, expected_words);
    emit_json(
        "{\"test\":\"lkl_ptrace_register_snapshot\",\"case\":\"%s\",\"target\":%d,"
        "\"mode\":%d,\"comparison_mask\":%llu,\"actual\":[%s],\"expected\":[%s],"
        "\"differences\":%llu,\"passed\":%s}\n",
        fixture->name, target, mode, (unsigned long long)mask,
        actual_text, expected_text, (unsigned long long)differences, differences ? "false" : "true");
    return check(fixture->name, "gpr_matches_assembly_snapshot", !differences, differences) &&
           check(fixture->name, "raw_sp_eight_byte_offset_preserved", (registers->sp & 15) == 8, registers->sp);
}

static int regset_lengths(struct fixture *fixture, pid_t target, unsigned long note,
                          const void *reference, size_t full_bytes)
{
    const size_t lengths[] = {0, 8, full_bytes, full_bytes + 8, SIZE_MAX - 7, 1, 9, full_bytes + 1};
    int passed = 1;
    for (unsigned index = 0; index < sizeof(lengths) / sizeof(lengths[0]); ++index) {
        unsigned char buffer[288];
        memset(buffer, 0xa5, sizeof(buffer));
        size_t length = lengths[index];
        void *base = length ? buffer : (void *)1;
        struct iovec vector = {base, length};
        int error = !regset_supported ? EIO : length % 8 ? EINVAL
                                                         : 0;
        char step[96];
        snprintf(step, sizeof(step), "getregset_%lx_length_%zu", note, length);
        passed &= request(fixture->name, step, PTRACE_GETREGSET, target, (void *)note, &vector, error);
        size_t copied = !error ? length < full_bytes ? length : full_bytes : 0;
        snprintf(step, sizeof(step), "getregset_%lx_length_%zu_output", note, length);
        passed &= check(fixture->name, step,
                        vector.iov_base == base && vector.iov_len == (!error ? copied : length) &&
                            !memcmp(buffer, reference, copied) && filled(buffer + copied, sizeof(buffer) - copied, 0xa5),
                        vector.iov_len);
    }
    return passed;
}

static int regset_faults(struct fixture *fixture, pid_t target, unsigned long note,
                         const void *reference, size_t full_bytes)
{
    char step[96];
    int error = regset_supported ? EFAULT : EIO;
    int passed = 1;
    snprintf(step, sizeof(step), "getregset_%lx_invalid_iovec", note);
    passed &= request(fixture->name, step, PTRACE_GETREGSET, target, (void *)note, NULL, error);
    struct iovec invalid_output = {NULL, 8};
    snprintf(step, sizeof(step), "getregset_%lx_invalid_output", note);
    passed &= request(fixture->name, step, PTRACE_GETREGSET, target, (void *)note, &invalid_output, error);
    passed &= check(fixture->name, "invalid_output_iovec_preserved",
                    invalid_output.iov_base == NULL && invalid_output.iov_len == 8, invalid_output.iov_len);
    unsigned char *pages = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED)
        return 0;
    memset(pages, 0xa5, 16384);
    if (mprotect(pages + 16384, 16384, PROT_NONE)) {
        munmap(pages, 32768);
        return 0;
    }
    unsigned char *prefix = pages + 16384 - 8;
    struct iovec crossing = {prefix, 16};
    snprintf(step, sizeof(step), "getregset_%lx_crossing_output", note);
    passed &= request(fixture->name, step, PTRACE_GETREGSET, target, (void *)note, &crossing, error);
    /* EFAULT 允许复制部分输出；可见的内容只能是完整前缀和原填充值。 */
    int prefix_valid = 0;
    unsigned changed = 0;
    for (unsigned bytes = 0; bytes <= (regset_supported ? 8U : 0U); ++bytes)
        prefix_valid |= !memcmp(prefix, reference, bytes) && filled(prefix + bytes, 8 - bytes, 0xa5);
    for (unsigned bytes = 0; bytes < 8; ++bytes)
        changed += prefix[bytes] != 0xa5;
    snprintf(step, sizeof(step), "getregset_%lx_crossing_output_range", note);
    passed &= check(fixture->name, step,
                    prefix_valid && filled(pages, 16384 - 8, 0xa5) &&
                        crossing.iov_base == prefix && crossing.iov_len == 16,
                    changed);
    unsigned char output[288];
    memset(output, 0xa5, sizeof(output));
    void *output_pointer = output;
    memcpy(prefix, &output_pointer, sizeof(output_pointer));
    snprintf(step, sizeof(step), "getregset_%lx_crossing_iovec", note);
    passed &= request(fixture->name, step, PTRACE_GETREGSET, target, (void *)note, prefix, error);
    passed &= check(fixture->name, "invalid_iovec_output_preserved", filled(output, sizeof(output), 0xa5), 0);
    passed &= mprotect(pages + 16384, 16384, PROT_READ | PROT_WRITE) == 0;
    passed &= munmap(pages, 32768) == 0;
    struct iovec *readonly = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (readonly == MAP_FAILED)
        return 0;
    *readonly = (struct iovec){output, full_bytes + 8};
    if (mprotect(readonly, 16384, PROT_READ)) {
        munmap(readonly, 16384);
        return 0;
    }
    snprintf(step, sizeof(step), "getregset_%lx_readonly_iov_len", note);
    passed &= request(fixture->name, step, PTRACE_GETREGSET, target, (void *)note, readonly, error);
    snprintf(step, sizeof(step), "getregset_%lx_readonly_iov_len_output", note);
    passed &= check(fixture->name, step,
                    readonly->iov_base == output && readonly->iov_len == full_bytes + 8 &&
                        (regset_supported ? !memcmp(output, reference, full_bytes) &&
                                                filled(output + full_bytes, sizeof(output) - full_bytes, 0xa5)
                                          : filled(output, sizeof(output), 0xa5)),
                    readonly->iov_len);
    passed &= mprotect(readonly, 16384, PROT_READ | PROT_WRITE) == 0;
    passed &= munmap(readonly, 16384) == 0;
    return passed;
}

static int regset_unsupported(struct fixture *fixture, pid_t target)
{
    const unsigned long notes[] = {1, 0x401, 2, 0x409, 0x40a, 0xdead};
    int passed = 1;
    for (unsigned index = 0; index < sizeof(notes) / sizeof(notes[0]); ++index) {
        char step[96];
        snprintf(step, sizeof(step), "setregset_%lx_invalid_iovec", notes[index]);
        passed &= request(fixture->name, step, PTRACE_SETREGSET, target, (void *)notes[index], (void *)1, EIO);
        if (index >= 2) {
            snprintf(step, sizeof(step), "getregset_%lx_invalid_iovec", notes[index]);
            passed &= request(fixture->name, step, PTRACE_GETREGSET, target, (void *)notes[index], (void *)1, EIO);
        }
    }
    return passed;
}

static int read_regsets(struct fixture *fixture, pid_t target, uint64_t expected_tls, int mode, int boundaries)
{
    struct arm64_registers registers, after;
    if (!get_gpr(fixture, target, &registers) || !gpr_snapshot(fixture, target, &registers, mode))
        return 0;
    uint64_t tls[2];
    memset(tls, 0xa5, sizeof(tls));
    struct iovec vector = {tls, sizeof(tls)};
    int passed = request(fixture->name, "getregset_tls_full", PTRACE_GETREGSET, target,
                         (void *)0x401, &vector, regset_supported ? 0 : EIO);
    passed &= check(fixture->name, "getregset_tls_full_value",
                    vector.iov_base == tls && vector.iov_len == sizeof(tls) &&
                        (regset_supported ? tls[0] == expected_tls && tls[1] == 0 : filled(tls, sizeof(tls), 0xa5)),
                    tls[0]);
    uint64_t short_tls = UINT64_C(0xa5a5a5a5a5a5a5a5);
    vector = (struct iovec){&short_tls, sizeof(short_tls)};
    passed &= request(fixture->name, "getregset_tls_short", PTRACE_GETREGSET, target,
                      (void *)0x401, &vector, regset_supported ? 0 : EIO);
    passed &= check(fixture->name, "getregset_tls_short_value",
                    vector.iov_base == &short_tls && vector.iov_len == 8 &&
                        short_tls == (regset_supported ? expected_tls : UINT64_C(0xa5a5a5a5a5a5a5a5)),
                    short_tls);
    if (boundaries) {
        passed &= regset_lengths(fixture, target, 1, &registers, sizeof(registers));
        passed &= regset_lengths(fixture, target, 0x401, tls, sizeof(tls));
        passed &= regset_faults(fixture, target, 1, &registers, sizeof(registers));
        passed &= regset_faults(fixture, target, 0x401, tls, sizeof(tls));
        passed &= regset_unsupported(fixture, target);
    }
    passed &= get_gpr(fixture, target, &after);
    passed &= check(fixture->name, "readonly_regset_target_preserved",
                    !memcmp(&registers, &after, sizeof(registers)), 0);
    return passed;
}

static int interrupt_case(int blocking)
{
    struct fixture fixture = {.name = blocking ? "blocking_read" : "running_user_loop"};
    int passed = 0;
    int ready = start(&fixture, blocking);
    if (!check(fixture.name, "child_ready", ready, fixture.leader))
        goto done;
    if (!request(fixture.name, "untraced_target", PTRACE_INTERRUPT, fixture.leader, NULL, NULL, ESRCH))
        goto done;
    if (!request(fixture.name, "seize", PTRACE_SEIZE, fixture.leader, NULL, NULL, 0))
        goto done;
    if (blocking) {
        if (!check(fixture.name, "read_blocked", wait_sleeping(&fixture), 0))
            goto done;
    } else if (write(fixture.gate, "\x5a", 1) != 1 ||
               !check(fixture.name, "loop_started", wait_running(&fixture, 0), 0)) {
        goto done;
    }
    if (!request(fixture.name, "interrupt", PTRACE_INTERRUPT, fixture.leader, NULL, NULL, 0) ||
        !stopped(fixture.name, "interrupt_stop", fixture.leader, PTRACE_EVENT_STOP))
        goto done;
    uint64_t counter = __atomic_load_n(&fixture.shared->counter, __ATOMIC_ACQUIRE);
    pause_ms(20);
    if (!check(fixture.name, "stopped_target_idle",
               counter == __atomic_load_n(&fixture.shared->counter, __ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&fixture.shared->returned, __ATOMIC_ACQUIRE),
               counter) ||
        !read_regsets(&fixture, fixture.leader, fixture.shared->tls_before, blocking ? 2 : 1, !blocking))
        goto done;
    pause_ms(20);
    if (!check(fixture.name, "regset_reads_kept_target_stopped",
               counter == __atomic_load_n(&fixture.shared->counter, __ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&fixture.shared->returned, __ATOMIC_ACQUIRE),
               counter))
        goto done;
    if (!request(fixture.name, "continue", PTRACE_CONT, fixture.leader, NULL, NULL, 0) ||
        !check(fixture.name, "continued_target", blocking ? wait_sleeping(&fixture) : wait_running(&fixture, counter), 0))
        goto done;
    if (!request(fixture.name, "interrupt_again", PTRACE_INTERRUPT, fixture.leader, NULL, NULL, 0) ||
        !stopped(fixture.name, "interrupt_stop_again", fixture.leader, PTRACE_EVENT_STOP) ||
        !read_regsets(&fixture, fixture.leader, fixture.shared->tls_before, blocking ? 2 : 1, 0) ||
        !request(fixture.name, "detach", PTRACE_DETACH, fixture.leader, NULL, NULL, 0))
        goto done;
    if (blocking) {
        int status = 0;
        if (write(fixture.gate, "\x5a", 1) != 1 ||
            wait_for(fixture.leader, &status, 0) != fixture.leader ||
            !check(fixture.name, "blocking_read_restarted", WIFEXITED(status) && WEXITSTATUS(status) == 42 && fixture.shared->returned == 1, status))
            goto done;
        fixture.leader = 0;
    } else if (!check(fixture.name, "detached_target_running",
                      wait_running(&fixture, __atomic_load_n(&fixture.shared->counter, __ATOMIC_ACQUIRE)), 0)) {
        goto done;
    }
    passed = 1;
done:
    return cleanup(&fixture) && passed;
}

static int clone_case(int settls)
{
    struct fixture fixture = {.name = settls ? "clone_settls" : "clone_event"};
    int passed = 0;
    int ready = start(&fixture, settls ? 3 : 2);
    if (!check(fixture.name, "child_ready", ready, fixture.leader) ||
        !request(fixture.name, "seize_traceclone", PTRACE_SEIZE, fixture.leader, NULL,
                 (void *)PTRACE_O_TRACECLONE, 0))
        goto done;
    if (write(fixture.gate, "\x5a", 1) != 1 ||
        !stopped(fixture.name, "clone_event_stop", fixture.leader, PTRACE_EVENT_CLONE))
        goto done;
    unsigned long child = 0;
    if (!request(fixture.name, "geteventmsg", PTRACE_GETEVENTMSG, fixture.leader,
                 NULL, &child, 0) ||
        !check(fixture.name, "actual_clone_pid", child > 0 && child <= INT32_MAX && child != (unsigned long)fixture.leader, child) ||
        !request(fixture.name, "geteventmsg_invalid_output", PTRACE_GETEVENTMSG,
                 fixture.leader, NULL, NULL, EFAULT) ||
        !stopped(fixture.name, "clone_child_stop", child, PTRACE_EVENT_STOP) ||
        !check(fixture.name, "clone_stopped_before_user_write",
               !__atomic_load_n(&fixture.shared->clone_entered, __ATOMIC_ACQUIRE),
               __atomic_load_n(&fixture.shared->clone_entered, __ATOMIC_ACQUIRE)) ||
        !read_regsets(&fixture, fixture.leader, fixture.shared->tls_before, 0, 0) ||
        !read_regsets(&fixture, child, settls ? fixture.shared->tls_requested : fixture.shared->tls_before, settls ? 3 : 0, 0) ||
        !check(fixture.name, "queue_blocked_clone_signal", !kill(child, SIGUSR1), child) ||
        !request(fixture.name, "continue_clone_child", PTRACE_CONT, child, NULL, NULL, 0))
        goto done;
    if (settls) {
        uint64_t deadline = now() + 5000000000;
        while (!__atomic_load_n(&fixture.shared->clone_entered, __ATOMIC_ACQUIRE) && now() < deadline)
            sched_yield();
        if (!check(fixture.name, "clone_resumed_with_requested_tls",
                   fixture.shared->clone_entered == 1 && fixture.shared->tls_after == fixture.shared->tls_requested,
                   fixture.shared->tls_after) ||
            !request(fixture.name, "interrupt_clone_child", PTRACE_INTERRUPT, child, NULL, NULL, 0) ||
            !stopped(fixture.name, "clone_child_second_stop", child, PTRACE_EVENT_STOP) ||
            !read_regsets(&fixture, child, fixture.shared->tls_requested, 4, 0) ||
            !request(fixture.name, "continue_clone_child_again", PTRACE_CONT, child, NULL, NULL, 0))
            goto done;
        __atomic_store_n(&fixture.shared->clone_release, 1, __ATOMIC_RELEASE);
    }
    int status = 0;
    if (wait_for(child, &status, __WALL) != (pid_t)child ||
        !check(fixture.name, "clone_child_exit", WIFEXITED(status) && WEXITSTATUS(status) == 43, status) ||
        !request(fixture.name, "detach_clone_parent", PTRACE_DETACH, fixture.leader, NULL, NULL, 0))
        goto done;
    if (wait_for(fixture.leader, &status, 0) != fixture.leader ||
        !check(fixture.name, "real_parent_waited_clone", WIFEXITED(status) && WEXITSTATUS(status) == 44, status))
        goto done;
    fixture.leader = 0;
    passed = 1;
done:
    return cleanup(&fixture) && passed;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(20);
    int passed = request("request_checks", "invalid_target", PTRACE_INTERRUPT, INT32_MAX, NULL, NULL, ESRCH);
    passed &= request("request_checks", "seize_self", PTRACE_SEIZE, getpid(), NULL, NULL, EPERM);
    passed &= interrupt_case(0);
    passed &= interrupt_case(1);
    passed &= clone_case(0);
    passed &= clone_case(1);
    alarm(0);
    emit_json("{\"test\":\"lkl_ptrace_complete\",\"passed\":%s}\n", passed ? "true" : "false");
    return passed ? 0 : 1;
}
