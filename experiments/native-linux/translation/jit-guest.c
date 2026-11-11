/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

static const char *jit_case;
static size_t page;
typedef unsigned (*Code)(void);

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            int error = errno; \
            dprintf(2, "standard-jit:%s:%d errno=%d\n", jit_case, __LINE__, error); \
            printf("{\"test\":\"linux_standard_jit\",\"case\":\"%s\",\"passed\":false,\"line\":%d,\"errno\":%d}\n", jit_case, __LINE__, error); \
            _exit(94); \
        } \
    } while (0)

static void constant(void *memory, unsigned value)
{
    uint32_t *words = memory;
    words[0] = 0x52800000 | (value << 5);
    words[1] = 0xd65f03c0;
}

static void clear_code(void *write, void *execute)
{
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t data_line = 4UL << ((ctr >> 16) & 15);
    size_t instruction_line = 4UL << (ctr & 15);
    for (uintptr_t at = (uintptr_t)write & -data_line; at < (uintptr_t)write + 8; at += data_line)
        asm volatile("dc cvau, %0" : : "r"(at) : "memory");
    asm volatile("dsb ish" : : : "memory");
    for (uintptr_t at = (uintptr_t)execute & -instruction_line; at < (uintptr_t)execute + 8; at += instruction_line)
        asm volatile("ic ivau, %0" : : "r"(at) : "memory");
    asm volatile("dsb ish\nisb" : : : "memory");
}

static void report(unsigned observed)
{
    printf("{\"test\":\"linux_standard_jit\",\"case\":\"%s\",\"passed\":true,\"observed\":%u}\n", jit_case, observed);
}

static void expect_value(unsigned observed, unsigned expected)
{
    if (observed != expected) {
        dprintf(2, "standard-jit:%s observed=%u expected=%u\n", jit_case, observed, expected);
        printf("{\"test\":\"linux_standard_jit\",\"case\":\"%s\",\"passed\":false,\"observed\":%u,\"expected\":%u}\n", jit_case, observed, expected);
        _exit(94);
    }
}

static unsigned direct_call(void *code)
{
    CHECK(syscall(SYS_getpid) > 0);
    return ((Code)code)();
}

static unsigned fallback_call(void *code)
{
    /* ISB enters Unicorn in the current Direct decoder. The following call
     * shares that fallback interval until the next syscall or budget exit. */
    asm volatile("isb" : : : "memory");
    return ((Code)code)();
}

static unsigned fallback_flush(void *code, void *target)
{
    asm volatile("isb" : : : "memory");
    return ((unsigned (*)(void *))code)(target);
}

struct Fault {
    int signal, code;
    uintptr_t address;
};
static int fault_output;

static void fault_handler(int number, siginfo_t *info, void *context)
{
    (void)context;
    struct Fault record = {number, info->si_code, (uintptr_t)info->si_addr};
    ssize_t count = write(fault_output, &record, sizeof(record));
    _exit(count == sizeof(record) ? 0 : 93);
}

static void wait_child(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void expect_fault(void *memory, int write_access, int expected)
{
    int descriptors[2];
    CHECK(pipe(descriptors) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(descriptors[0]);
        fault_output = descriptors[1];
        struct sigaction action = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
        sigemptyset(&action.sa_mask);
        CHECK(sigaction(SIGSEGV, &action, NULL) == 0);
        if (write_access)
            *(volatile uint32_t *)memory = 0;
        else
            ((Code)memory)();
        _exit(92);
    }
    CHECK(close(descriptors[1]) == 0);
    struct Fault record = {0};
    CHECK(read(descriptors[0], &record, sizeof(record)) == sizeof(record));
    CHECK(close(descriptors[0]) == 0);
    wait_child(child);
    CHECK(record.signal == SIGSEGV && record.code == expected && record.address == (uintptr_t)memory);
    printf("{\"test\":\"linux_standard_jit\",\"case\":\"%s\",\"passed\":true,\"signal\":%d,\"si_code\":%d,\"address\":%lu,\"expected_address\":%lu}\n",
           jit_case, record.signal, record.code, (unsigned long)record.address, (unsigned long)memory);
}

static void anonymous_code(void)
{
    jit_case = "anon_rw_rx";
    void *memory = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    constant(memory, 42);
    clear_code(memory, memory);
    CHECK(mprotect(memory, page, PROT_READ | PROT_EXEC) == 0);
    unsigned result = direct_call(memory);
    CHECK(result == 42);
    report(result);

    jit_case = "anon_rewrite";
    CHECK(mprotect(memory, page, PROT_READ | PROT_WRITE) == 0);
    constant(memory, 91);
    clear_code(memory, memory);
    CHECK(mprotect(memory, page, PROT_READ | PROT_EXEC) == 0);
    CHECK((result = direct_call(memory)) == 91);
    report(result);

    jit_case = "rx_write";
    expect_fault(memory, 1, SEGV_ACCERR);
    jit_case = "remove_execute";
    CHECK(mprotect(memory, page, PROT_READ) == 0);
    expect_fault(memory, 0, SEGV_ACCERR);
    CHECK(mprotect(memory, page, PROT_READ | PROT_EXEC) == 0);

    jit_case = "mremap_move";
    void *destination = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(destination != MAP_FAILED && destination != memory);
    CHECK(mremap(memory, page, page, MREMAP_MAYMOVE | MREMAP_FIXED, destination) == destination);
    CHECK((result = direct_call(destination)) == 91);
    report(result);
    jit_case = "mremap_old_unmapped";
    expect_fault(memory, 0, SEGV_MAPERR);
    jit_case = "unmapped_execute";
    CHECK(munmap(destination, page) == 0);
    expect_fault(destination, 0, SEGV_MAPERR);

    jit_case = "anon_rx_map";
    memory = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED && munmap(memory, page) == 0);
    report(1);
    jit_case = "anon_rwx";
    memory = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    constant(memory, 73);
    clear_code(memory, memory);
    CHECK((result = direct_call(memory)) == 73);
    constant(memory, 74);
    clear_code(memory, memory);
    CHECK((result = direct_call(memory)) == 74);
    CHECK(munmap(memory, page) == 0);
    report(result);

    jit_case = "exec_only_vma";
    memory = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    constant(memory, 58);
    CHECK(mprotect(memory, page, PROT_EXEC) == 0);
    clear_code(memory, memory);
    CHECK((result = direct_call(memory)) == 58);
    CHECK(munmap(memory, page) == 0);
    report(result);
}

static int code_file(size_t size)
{
    int fd = memfd_create("standard-jit", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_EXEC);
    CHECK(fd >= 0 && ftruncate(fd, size) == 0);
    return fd;
}

static void ordinary_file(void)
{
    jit_case = "ordinary_file_rx";
    char name[] = "jit-code-XXXXXX";
    int fd = mkstemp(name);
    uint32_t words[2];
    constant(words, 57);
    CHECK(fd >= 0 && ftruncate(fd, page) == 0);
    CHECK(pwrite(fd, words, sizeof(words), 0) == sizeof(words));
    CHECK(close(fd) == 0);
    fd = open(name, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && unlink(name) == 0);
    void *execute = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    CHECK(execute != MAP_FAILED && direct_call(execute) == 57);
    CHECK(munmap(execute, page) == 0);
    report(57);
    jit_case = "readonly_shared_write";
    errno = 0;
    CHECK(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES);
    CHECK(close(fd) == 0);
    report(EACCES);
}

static void noexec_file(void)
{
    jit_case = "noexec_mmap";
    char directory[] = "jit-noexec-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(mount("tmpfs", directory, "tmpfs", MS_NOEXEC, "size=64k") == 0);
    char name[64];
    int length = snprintf(name, sizeof(name), "%s/code", directory);
    CHECK(length >= 0 && (size_t)length < sizeof(name));
    int fd = open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && ftruncate(fd, page) == 0);
    errno = 0;
    CHECK(mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EPERM);
    report(EPERM);
    jit_case = "noexec_mprotect";
    void *memory = mmap(NULL, page, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(memory != MAP_FAILED);
    errno = 0;
    CHECK(mprotect(memory, page, PROT_READ | PROT_EXEC) == -1 && errno == EACCES);
    CHECK(munmap(memory, page) == 0 && close(fd) == 0 && unlink(name) == 0);
    CHECK(umount(directory) == 0 && rmdir(directory) == 0);
    report(EACCES);
}

static void unpopulated_exec_alias(void)
{
    jit_case = "unpopulated_exec_alias";
    int fd = code_file(2 * page);
    void *write = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
    void *execute = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, page);
    CHECK(write != MAP_FAILED && execute != MAP_FAILED && write != execute);
    constant(write, 59);
    /* The executable alias has had no load or fetch before cache maintenance. */
    clear_code(write, execute);
    CHECK(direct_call(execute) == 59);
    CHECK(munmap(write, page) == 0 && munmap(execute, page) == 0 && close(fd) == 0);
    report(59);
}

static void copy_on_write(int fd)
{
    jit_case = fd < 0 ? "private_cow_parent" : "private_file_cow";
    void *memory = mmap(NULL, page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | (fd < 0 ? MAP_ANONYMOUS : 0), fd, 0);
    CHECK(memory != MAP_FAILED);
    if (fd < 0)
        constant(memory, 41);
    CHECK(*(uint32_t *)memory == (0x52800000 | (41 << 5)));
    clear_code(memory, memory);
    CHECK(mprotect(memory, page, PROT_READ | PROT_EXEC) == 0 && direct_call(memory) == 41);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(mprotect(memory, page, PROT_READ | PROT_WRITE) == 0);
        constant(memory, 69);
        clear_code(memory, memory);
        CHECK(mprotect(memory, page, PROT_READ | PROT_EXEC) == 0 && direct_call(memory) == 69);
        _exit(0);
    }
    wait_child(child);
    CHECK(direct_call(memory) == 41 && munmap(memory, page) == 0);
    if (fd >= 0) {
        uint32_t word;
        CHECK(pread(fd, &word, sizeof(word), 0) == sizeof(word));
        CHECK(word == (0x52800000 | (41 << 5)));
    }
    report(41);
}

static void shared_code(void)
{
    jit_case = "memfd_alias_updates";
    int fd = code_file(3 * page);
    uint32_t original[2];
    constant(original, 41);
    CHECK(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
    copy_on_write(fd);
    jit_case = "memfd_alias_updates";
    void *write = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
    void *execute = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, page);
    CHECK(write != MAP_FAILED && execute != MAP_FAILED && write != execute);
    for (unsigned i = 0; i < 16; ++i) {
        constant(write, 50 + i);
        clear_code(write, execute);
        CHECK(direct_call(execute) == 50 + i);
    }
    report(16);
    jit_case = "memfd_pwrite";
    uint32_t words[2];
    constant(words, 97);
    CHECK(pwrite(fd, words, sizeof(words), page) == sizeof(words));
    clear_code(write, execute);
    CHECK(direct_call(execute) == 97);
    report(97);

    jit_case = "memfd_shared_fork";
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        constant(write, 111);
        clear_code(write, execute);
        CHECK(direct_call(execute) == 111);
        _exit(0);
    }
    wait_child(child);
    CHECK(direct_call(execute) == 111);
    report(111);
    jit_case = "memfd_mremap_clone";
    void *clone = mremap(execute, 0, page, MREMAP_MAYMOVE);
    CHECK(clone != MAP_FAILED && clone != execute);
    CHECK(direct_call(clone) == 111 && direct_call(execute) == 111);
    CHECK(munmap(clone, page) == 0);
    report(111);

    jit_case = "unicorn_alias_update";
    CHECK(fallback_call(execute) == 111);
    constant(write, 201);
    clear_code(write, execute);
    CHECK(fallback_call(execute) == 201);
    report(201);
    jit_case = "unicorn_pwrite_update";
    constant(words, 202);
    CHECK(pwrite(fd, words, sizeof(words), page) == sizeof(words));
    clear_code(write, execute);
    expect_value(fallback_call(execute), 202);
    report(202);

    jit_case = "unicorn_unseen_pwrite";
    void *unseen_write = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 2 * page);
    void *unseen_execute = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 2 * page);
    CHECK(unseen_write != MAP_FAILED && unseen_execute != MAP_FAILED);
    constant(unseen_write, 63);
    /* Fault in the executable PTE through a read before entering Unicorn, so
     * its first fetch need not return to Direct for a Linux page fault. */
    CHECK(*(volatile uint32_t *)unseen_execute == (0x52800000 | (63 << 5)));
    clear_code(unseen_write, unseen_execute);
    CHECK(syscall(SYS_getpid) > 0 && fallback_call(unseen_execute) == 63);
    constant(words, 203);
    CHECK(pwrite(fd, words, sizeof(words), 2 * page) == sizeof(words));
    clear_code(unseen_write, unseen_execute);
    expect_value(fallback_call(unseen_execute), 203);
    report(203);
    CHECK(munmap(unseen_execute, page) == 0 && munmap(unseen_write, page) == 0);

    jit_case = "same_va_new_backing";
    CHECK(munmap(execute, page) == 0);
    int other = code_file(page);
    constant(words, 87);
    CHECK(pwrite(other, words, sizeof(words), 0) == sizeof(words));
    CHECK(mmap(execute, page, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED_NOREPLACE, other, 0) == execute);
    CHECK(direct_call(execute) == 87);
    CHECK(munmap(execute, page) == 0 && close(other) == 0 && munmap(write, page) == 0 && close(fd) == 0);
    report(87);

    jit_case = "sealed_write";
    fd = code_file(page);
    CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == 0);
    errno = 0;
    CHECK(pwrite(fd, words, sizeof(words), 0) == -1 && errno == EPERM);
    CHECK(close(fd) == 0);
    report(EPERM);
}

static void flush_executing_page(void)
{
    jit_case = "unicorn_self_page_flush";
    int fd = code_file(page);
    uint32_t code[18] = {
        0xd50b7b20,
        0xd5033b9f,
        0xd50b7520,
        0xd5033b9f,
        0xd5033fdf,
        0x1400000b,
    };
    /* dc cvau,x0; dsb ish; ic ivau,x0; dsb ish; isb; b target.
     * The target shares the current TB's physical page. */
    constant(code + 16, 11);
    CHECK(pwrite(fd, code, sizeof(code), 0) == sizeof(code));
    void *execute = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    CHECK(execute != MAP_FAILED && *(volatile uint32_t *)execute == code[0]);
    CHECK(syscall(SYS_getpid) > 0);
    expect_value(fallback_flush(execute, (char *)execute + 64), 11);
    constant(code + 16, 67);
    CHECK(pwrite(fd, code + 16, 8, 64) == 8);
    expect_value(fallback_flush(execute, (char *)execute + 64), 67);
    CHECK(munmap(execute, page) == 0 && close(fd) == 0);
    report(67);
}

void standard_jit_tests(void)
{
    jit_case = "configuration";
    long size = sysconf(_SC_PAGESIZE);
    CHECK(size > 0);
    page = (size_t)size;
    printf("{\"config\":\"linux_standard_jit\",\"page_bytes\":%zu}\n", page);
    anonymous_code();
    ordinary_file();
    noexec_file();
    unpopulated_exec_alias();
    copy_on_write(-1);
    shared_code();
    flush_executing_page();
}

#ifdef NCE_JIT_STANDALONE
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    standard_jit_tests();
    return 0;
}
#endif
