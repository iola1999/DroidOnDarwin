/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
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

struct shared {
    uint64_t counter;
    unsigned returned, clone_entered;
};

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

static int check(const char *name, const char *step, int passed, long observed)
{
    printf("{\"test\":\"lkl_ptrace\",\"case\":\"%s\",\"step\":\"%s\",\"observed\":%ld,\"passed\":%s}\n",
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
    printf(
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
        if (write(ready[1], "1", 1) != 1)
            _exit(121);
        close(ready[1]);
        unsigned char command = 0;
        errno = 0;
        ssize_t result = read(gate[0], &command, 1);
        if (mode == 1)
            printf(
                "{\"test\":\"lkl_ptrace\",\"case\":\"blocking_read\",\"step\":\"user_read_return\","
                "\"result\":%ld,\"errno\":%d,\"passed\":%s}\n",
                (long)result, errno,
                result == 1 && command == 0x5a ? "true" : "false");
        if (result != 1 || command != 0x5a)
            _exit(122);
        if (mode == 0) {
            for (;;)
                __atomic_fetch_add(&fixture->shared->counter, 1, __ATOMIC_RELAXED);
        } else if (mode == 1) {
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
        pid_t clone_pid = syscall(SYS_clone, (unsigned long)CLONE_FILES, 0L, 0L, 0L, 0L);
        if (clone_pid < 0)
            _exit(123);
        if (!clone_pid) {
            __atomic_store_n(&fixture->shared->clone_entered, 1, __ATOMIC_RELEASE);
            uintptr_t tls_after, sp_after, fp_after;
            __asm__ volatile("mrs %0, tpidr_el0\nmov %1, sp\nmov %2, x29"
                             : "=r"(tls_after), "=r"(sp_after), "=r"(fp_after));
            int child_passed = tls_before == tls_after && sp_before == sp_after && fp_before == fp_after;
            syscall(SYS_getpid);
            child_passed = check(fixture->name, "clone_tls_stack_frame_preserved", child_passed, tls_after);
            sigset_t mask, pending;
            child_passed &= check(fixture->name, "clone_signal_mask_preserved",
                                  !sigprocmask(SIG_SETMASK, NULL, &mask) && sigismember(&mask, SIGUSR1) == 1, 0);
            child_passed &= check(fixture->name, "clone_blocked_signal_pending",
                                  !sigpending(&pending) && sigismember(&pending, SIGUSR1) == 1, 0);
            _exit(child_passed ? 43 : 125);
        }
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

static int regset_unimplemented(struct fixture *fixture)
{
    unsigned char registers[272], before[272];
    memset(registers, 0xa5, sizeof(registers));
    memcpy(before, registers, sizeof(before));
    struct iovec vector = {registers, sizeof(registers)};
    int passed = request(fixture->name, "getregset_unimplemented", PTRACE_GETREGSET,
                         fixture->leader, (void *)1, &vector, EIO);
    passed &= request(fixture->name, "setregset_unimplemented", PTRACE_SETREGSET,
                      fixture->leader, (void *)1, &vector, EIO);
    passed &= check(fixture->name, "regset_failed_output_preserved",
                    !memcmp(registers, before, sizeof(before)) &&
                        vector.iov_base == registers && vector.iov_len == sizeof(registers),
                    vector.iov_len);
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
        !regset_unimplemented(&fixture))
        goto done;
    if (!request(fixture.name, "continue", PTRACE_CONT, fixture.leader, NULL, NULL, 0) ||
        !check(fixture.name, "continued_target", blocking ? wait_sleeping(&fixture) : wait_running(&fixture, counter), 0))
        goto done;
    if (!request(fixture.name, "interrupt_again", PTRACE_INTERRUPT, fixture.leader, NULL, NULL, 0) ||
        !stopped(fixture.name, "interrupt_stop_again", fixture.leader, PTRACE_EVENT_STOP) ||
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

static int clone_case(void)
{
    struct fixture fixture = {.name = "clone_event"};
    int passed = 0;
    int ready = start(&fixture, 2);
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
        !regset_unimplemented(&fixture) ||
        !check(fixture.name, "queue_blocked_clone_signal", !kill(child, SIGUSR1), child) ||
        !request(fixture.name, "continue_clone_child", PTRACE_CONT, child, NULL, NULL, 0))
        goto done;
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
    passed &= clone_case();
    alarm(0);
    printf("{\"test\":\"lkl_ptrace_complete\",\"passed\":%s}\n", passed ? "true" : "false");
    return passed ? 0 : 1;
}
