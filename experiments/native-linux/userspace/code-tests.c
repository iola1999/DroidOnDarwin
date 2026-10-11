/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "../layout.h"
#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/auxv.h>
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
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "code-tests:%d errno=%d\n", __LINE__, errno); \
            _exit(96); \
        } \
    } while (0)
typedef uint64_t (*Code)(uint64_t);
static volatile sig_atomic_t ticks;
static void tick(int number)
{
    (void)number;
    ++ticks;
}

static void constant(uint32_t *rw, unsigned value)
{
    rw[0] = 0x52800000 | (value << 5); /* mov w0, value */
    rw[1] = 0xd65f03c0;
}

static void expect_fault(Code code, int write_access, int expected)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (write_access)
            *(volatile uint32_t *)code = 0;
        else
            code(0);
        _exit(99);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != expected)
        dprintf(2, "code fault expected=%d status=%d write=%d\n", expected, status, write_access);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == expected);
}

static void reserved_stubs(void)
{
    Elf64_Ehdr header;
    int file = open("/proc/self/exe", O_RDONLY);
    CHECK(file >= 0 && read(file, &header, sizeof(header)) == sizeof(header) && close(file) == 0);
    const Elf64_Phdr *phdrs = (const void *)getauxval(AT_PHDR);
    uintptr_t base = (uintptr_t)phdrs - header.e_phoff, end = 0;
    for (unsigned i = 0; i < header.e_phnum; ++i)
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr + phdrs[i].p_memsz > end)
            end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
    void *reserved = (void *)(base + ((end + PAGE - 1) & -PAGE));
    CHECK(mmap(reserved, PAGE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED &&
          errno == EACCES);
    CHECK(mprotect(reserved, PAGE, PROT_NONE) == -1 && errno == EACCES);
    void *data = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(data != MAP_FAILED);
    CHECK(mremap(data, PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, reserved) == MAP_FAILED && errno == EACCES);
    CHECK(munmap(data, PAGE) == 0 && munmap(reserved, PAGE) == 0);
    CHECK(getpid() > 0);
    puts("{\"test\":\"linux_reserved_branch_stubs\",\"passed\":true}");
}

static void simd_fallback_tests(void);

void dynamic_code_tests(void)
{
    simd_fallback_tests();
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
    rw[0] = 0; /* Guest UDF must remain SIGILL after lazy code restoration. */
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 4) == 0);
    expect_fault(rx, 0, SIGILL);
    constant(rw, 127);
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 8) == 0);
    expect_fault(rx, 1, SIGSEGV);
    CHECK(mprotect((void *)rx, PAGE, PROT_NONE) == 0);
    expect_fault(rx, 0, SIGSEGV);
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
        if (other != second)
            dprintf(2, "dynamic replacement=%p expected=%p errno=%d\n", (void *)other, (void *)second, errno);
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
    expect_fault(rx, 0, SIGSEGV);
    CHECK(syscall(NCE_SYS_PUBLISH_CODE, rx, 8) == -1 && errno == EFAULT);
    CHECK(munmap((void *)second, PAGE) == 0 && munmap(rw, 2 * PAGE) == 0 && close(fd) == 0);
    puts("{\"test\":\"linux_dynamic_code\",\"passed\":true,\"updates\":128,\"sum_iterations\":10000000}");
}

/* These instructions enter Unicorn when the Direct SIMD group is uncovered.
 * The following stores check continuation, including an interrupted exclusive
 * sequence whose retry must execute through its successful store. */
static void simd_fallback_tests(void)
{
    const float scale[2] = {2, 3}, input[2] = {4, 5};
    float product[2] = {0};
    const int32_t left[2] = {INT32_MIN, 8}, right[2] = {1, -5};
    int64_t difference[2] = {0};
    asm volatile(
        "ldr d0, [%0]\nldr d2, [%1]\n"
        ".inst 0x0f809040\nstr d0, [%2]"
        : : "r"(scale), "r"(input), "r"(product) : "v0", "v2", "memory");
    CHECK(product[0] == 8 && product[1] == 10);
    asm volatile(
        "ldr d0, [%0]\nldr d1, [%1]\n"
        ".inst 0x0ea12000\nstr q0, [%2]"
        : : "r"(left), "r"(right), "r"(difference) : "v0", "v1", "memory");
    CHECK(difference[0] == INT64_C(-2147483649) && difference[1] == 13);
    uint64_t fpcr, fpsr, flags;
    const uint32_t huge[2] = {0x7f7fffff, 0x7f7fffff};
    uint32_t overflow[2] = {0};
    asm volatile("mrs %0, fpcr\nmrs %1, fpsr" : "=r"(fpcr), "=r"(fpsr));
    asm volatile("msr fpcr, xzr\nmsr fpsr, xzr");
    CHECK(syscall(SYS_getpid) > 0);
    asm volatile(
        "ldr d0, [%1]\nldr d2, [%2]\n.inst 0x0f809040\n"
        "str d0, [%3]\nmrs %0, fpsr"
        : "=r"(flags) : "r"(scale), "r"(huge), "r"(overflow) : "v0", "v2", "memory");
    asm volatile("msr fpcr, %0\nmsr fpsr, %1" : : "r"(fpcr), "r"(fpsr));
    CHECK(overflow[0] == 0x7f800000 && overflow[1] == 0x7f800000 && (flags & 0x14) == 0x14);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint32_t cell = 41, status;
        CHECK(syscall(SYS_getpid) > 0);
        asm volatile(
            "ldr d0, [%2]\nldr d2, [%3]\nmov w11, #1024\n"
            "1: ldaxr w9, [%1]\n.inst 0x0f809040\nadd w9, w9, #1\n"
            "stlxr w10, w9, [%1]\ncbz w10, 2f\nsubs w11, w11, #1\nb.ne 1b\n"
            "2: mov %w0, w10\nclrex"
            : "=r"(status) : "r"(&cell), "r"(scale), "r"(input)
            : "x9", "x10", "x11", "v0", "v2", "cc", "memory");
        CHECK(status == 0 && cell == 42);
    }
    puts("{\"test\":\"linux_simd_fallback_continuation\",\"passed\":true}");
}

void code_tests(void)
{
    reserved_stubs();
    dynamic_code_tests();
}
