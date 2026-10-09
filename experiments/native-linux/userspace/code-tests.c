/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "../layout.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE NCE_PAGE_SIZE
#define CHECK(c) do { if (!(c)) { dprintf(2, "code-tests:%d errno=%d\n", __LINE__, errno); _exit(96); } } while (0)
typedef uint64_t (*Code)(uint64_t);
static volatile sig_atomic_t ticks;
static void tick(int number) { (void)number; ++ticks; }

static void constant(uint32_t *rw, unsigned value)
{
    rw[0] = 0x52800000 | (value << 5); /* mov w0, value */
    rw[1] = 0xd65f03c0;
}

static void expect_fault(Code code, int write_access)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (write_access) *(volatile uint32_t *)code = 0;
        else code(0);
        _exit(99);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

void code_tests(void)
{
    CHECK(syscall(NCE_SYS_MAP_CODE, -1, PAGE, 0) == -1 && errno == EBADF);
    int fd = memfd_create("nce-code", MFD_CLOEXEC);
    CHECK(fd >= 0 && ftruncate(fd, 2 * PAGE) == 0);
    CHECK(syscall(NCE_SYS_MAP_CODE, fd, 0, 0) == -1 && errno == EINVAL);
    CHECK(syscall(NCE_SYS_MAP_CODE, fd, PAGE, 1) == -1 && errno == EINVAL);
    CHECK(syscall(NCE_SYS_MAP_CODE, fd, 3 * PAGE, 0) == -1 && errno == EINVAL);
    uint32_t *rw = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(rw != MAP_FAILED);
    CHECK(mprotect(rw, PAGE, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
    constant(rw, 42);
    constant(rw + PAGE / 4, 71);
    Code rx = (Code)syscall(NCE_SYS_MAP_CODE, fd, PAGE, 0);
    Code second = (Code)syscall(NCE_SYS_MAP_CODE, fd, PAGE, PAGE);
    CHECK(rx != (Code)MAP_FAILED && second != (Code)MAP_FAILED && rx(0) == 42 && second(0) == 71);
    CHECK(mmap((void *)rx, PAGE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0) == MAP_FAILED && errno == EACCES);
    CHECK(mprotect((void *)rx, PAGE, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    CHECK(mremap((void *)rx, PAGE, PAGE * 2, MREMAP_MAYMOVE) == MAP_FAILED && errno == EACCES);
    CHECK(mremap((void *)rx, 0, PAGE, MREMAP_MAYMOVE) == MAP_FAILED && errno == EACCES);
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rw, 8) == -1 && errno == EINVAL);
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, UINT64_MAX) == -1 && errno == EINVAL);
    for (unsigned i = 0; i < 128; ++i) {
        constant(rw, i);
        CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 8) == 0 && rx(0) == i);
    }
    expect_fault(rx, 1);
    CHECK(mprotect((void *)rx, PAGE, PROT_NONE) == 0);
    expect_fault(rx, 0);
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 8) == -1 && errno == EFAULT);
    CHECK(mprotect((void *)rx, PAGE, PROT_READ | PROT_EXEC) == 0 && rx(0) == 127);

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 8) == 0);
        CHECK(rx(0) == 127);
        constant(rw, 201);
        CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 8) == 0 && rx(0) == 201);
        CHECK(munmap((void *)second, PAGE) == 0);
        int replacement = memfd_create("nce-child-code", MFD_CLOEXEC);
        CHECK(replacement >= 0 && ftruncate(replacement, PAGE) == 0);
        uint32_t value[] = {0x52800b20, 0xd65f03c0}; /* mov w0, 89; ret */
        CHECK(pwrite(replacement, value, sizeof(value), 0) == sizeof(value));
        Code other = (Code)syscall(NCE_SYS_MAP_CODE, replacement, PAGE, 0);
        if (other != second) dprintf(2, "dynamic replacement=%p expected=%p errno=%d\n", (void *)other, (void *)second, errno);
        CHECK(other == second && other(0) == 89);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    CHECK(rx(0) == 201 && second(0) == 71);
    /* This loop runs long enough for the host timer to enter and resume code
     * that has no ELF metadata or syscall patch table. */
    const uint32_t sum[] = {0xd2800001, 0x8b000021, 0xf1000400, 0x54ffffc1, 0xaa0103e0, 0xd65f03c0};
    memcpy(rw, sum, sizeof(sum));
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, sizeof(sum)) == 0);
    CHECK(rx(10000000) == UINT64_C(50000005000000));
    /* Wait inside generated code until a Linux signal handler updates memory.
     * Completion therefore requires signal delivery and a return to this PC. */
    const uint32_t wait_signal[] = {0xb9400001, 0x34ffffe1, 0xd2800920, 0xd65f03c0};
    memcpy(rw, wait_signal, sizeof(wait_signal));
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, sizeof(wait_signal)) == 0);
    CHECK(signal(SIGALRM, tick) != SIG_ERR);
    struct itimerval timer = {.it_value = {.tv_usec = 1000}};
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    CHECK(rx((uintptr_t)&ticks) == 73);
    memset(&timer, 0, sizeof(timer));
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0 && ticks > 0);
    CHECK(munmap((void *)rx, PAGE) == 0);
    expect_fault(rx, 0);
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 8) == -1 && errno == EFAULT);
    CHECK(munmap((void *)second, PAGE) == 0 && munmap(rw, 2 * PAGE) == 0 && close(fd) == 0);
    puts("{\"test\":\"linux_dynamic_code\",\"passed\":true,\"updates\":128,\"sum_iterations\":10000000}");
}
