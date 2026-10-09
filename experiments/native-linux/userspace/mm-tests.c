/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 16384
#define CHECK(c) do { if (!(c)) { dprintf(2, "mm-tests:%d errno=%d\n", __LINE__, errno); _exit(__LINE__); } } while (0)

__attribute__((noinline)) static unsigned grow_stack(unsigned depth)
{
    volatile unsigned char page[PAGE];
    /* Keep the complete stack allocation observable to the compiler. */
    __asm__ volatile("" : : "r"(page) : "memory");
    page[0] = depth;
    page[PAGE - 1] = depth + 1;
    unsigned result = depth ? grow_stack(depth - 1) : 0;
    return result + page[0] + page[PAGE - 1];
}

void file_tests(const char *libc);

void memory_tests(void)
{
    /* Raw brk changes precede libc's first allocation so its arena stays intact. */
    unsigned char *heap = (void *)syscall(SYS_brk, 0);
    CHECK((void *)syscall(SYS_brk, heap + 8 * PAGE) == heap + 8 * PAGE);
    for (unsigned i = 0; i < 8; ++i) heap[i * PAGE] = i + 1;
    pid_t heap_child = fork();
    CHECK(heap_child >= 0);
    if (!heap_child) {
        for (unsigned i = 0; i < 8; ++i) {
            CHECK(heap[i * PAGE] == i + 1);
            heap[i * PAGE] = i + 17;
        }
        _exit(0);
    }
    int heap_status;
    CHECK(waitpid(heap_child, &heap_status, 0) == heap_child && heap_status == 0);
    for (unsigned i = 0; i < 8; ++i) CHECK(heap[i * PAGE] == i + 1);
    CHECK((void *)syscall(SYS_brk, heap) == heap);
    puts("{\"test\":\"linux_brk_growth_cow\",\"passed\":true}");
    file_tests("musl");
    pid_t limited = fork();
    CHECK(limited >= 0);
    if (!limited) {
        struct rlimit limit = {128 * 1024, 128 * 1024};
        CHECK(setrlimit(RLIMIT_STACK, &limit) == 0);
        _exit(grow_stack(64) == 4225 ? 0 : 1);
    }
    int limited_status;
    CHECK(waitpid(limited, &limited_status, 0) == limited);
    CHECK(WIFSIGNALED(limited_status) && WTERMSIG(limited_status) == SIGSEGV);
    CHECK(grow_stack(64) == 4225);
    puts("{\"test\":\"linux_stack_growth_and_limit\",\"passed\":true}");
    CHECK(syscall(245, -1, PAGE) == -1 && errno == EBADF);
    int ordinary = open("/reserve-not-elf", O_CREAT | O_RDWR, 0600);
    CHECK(ordinary >= 0);
    CHECK(syscall(245, ordinary, PAGE) == -1 && errno == ENOEXEC);
    CHECK(close(ordinary) == 0);
    int elf = open("/bionic-test", O_RDONLY);
    CHECK(elf >= 0);
    Elf64_Ehdr header;
    CHECK(pread(elf, &header, sizeof(header), 0) == sizeof(header));
    size_t load_size = 0;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr phdr;
        CHECK(pread(elf, &phdr, sizeof(phdr), header.e_phoff + i * sizeof(phdr)) == sizeof(phdr));
        if (phdr.p_type == PT_LOAD && phdr.p_vaddr + phdr.p_memsz > load_size)
            load_size = phdr.p_vaddr + phdr.p_memsz;
    }
    load_size = (load_size + PAGE - 1) & ~(size_t)(PAGE - 1);
    CHECK(syscall(245, elf, load_size + PAGE) == -1 && errno == EINVAL);
    void *elf_reservation = (void *)syscall(245, elf, load_size);
    CHECK(elf_reservation != MAP_FAILED);
    CHECK(syscall(245, elf, load_size) == -1 && errno == EEXIST);
    void *metadata = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, elf, 0);
    CHECK(metadata != MAP_FAILED && metadata != elf_reservation);
    CHECK(!memcmp(metadata, ELFMAG, SELFMAG));
    CHECK(munmap(metadata, PAGE) == 0 && munmap(elf_reservation, load_size) == 0);
    CHECK(close(elf) == 0);
    puts("{\"test\":\"linux_android_elf_reservation_boundaries\",\"passed\":true}");

    const size_t reserve = 2UL * 1024 * 1024 * 1024;
    unsigned char *sparse = mmap(NULL, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(sparse != MAP_FAILED);
    const size_t offsets[] = {0, 1024UL * 1024 * 1024, reserve - PAGE};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(mprotect(sparse + offsets[i], PAGE, PROT_READ | PROT_WRITE) == 0);
        sparse[offsets[i]] = 17 + i;
    }
    pid_t sparse_child = fork();
    CHECK(sparse_child >= 0);
    if (!sparse_child) {
        for (unsigned i = 0; i < 3; ++i) {
            CHECK(sparse[offsets[i]] == 17 + i);
            sparse[offsets[i]] = 90 + i;
        }
        _exit(0);
    }
    int sparse_status;
    CHECK(waitpid(sparse_child, &sparse_status, 0) == sparse_child && sparse_status == 0);
    for (unsigned i = 0; i < 3; ++i) CHECK(sparse[offsets[i]] == 17 + i);
    CHECK(munmap(sparse, reserve) == 0);
    printf("{\"test\":\"linux_sparse_reservation_cow\",\"passed\":true,\"reserved_bytes\":%zu}\n", reserve);

    unsigned char *data = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(data != MAP_FAILED);
    int fd = open("/memory-tests", O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(fd >= 0);
    memset(data, 17, 4 * PAGE);
    struct iovec vec[] = {{NULL, 0}, {data, 32}};
    CHECK(writev(fd, vec, 2) == 32);
    CHECK(syscall(SYS_write, fd, 0, 1) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_write, fd, UINTPTR_MAX - 7, 32) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_openat, AT_FDCWD, 1, O_RDONLY, 0) == -1 && errno == EFAULT);
    CHECK(syscall(246, 0, 0) == -1 && errno == ENOSYS);
    CHECK(mprotect(data + PAGE, PAGE, PROT_NONE) == 0);
    CHECK(syscall(SYS_write, fd, data + PAGE, 1) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_futex, data + PAGE, 128, 0, 0, 0, 0) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_write, fd, data + PAGE - 16, 32) == 16);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, data + PAGE - 16, 32) == 16);
    CHECK(mprotect(data, PAGE, PROT_READ) == 0);
    CHECK(pread(fd, data, 1, 0) == -1 && errno == EFAULT);
    CHECK(write(fd, data, 1) == 1);
    CHECK(mprotect(data, PAGE, PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect(data + PAGE, PAGE, PROT_READ | PROT_WRITE) == 0);
    CHECK(ftruncate(fd, 2 * PAGE) == 0);
    unsigned char marker = 0x6b;
    CHECK(pwrite(fd, &marker, 1, PAGE) == 1);
    unsigned char *second = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
    CHECK(second != MAP_FAILED && second[0] == marker && second[1] == 0);
    CHECK(munmap(second, PAGE) == 0);
    CHECK(mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE + 1) == MAP_FAILED && errno == EINVAL);
    puts("{\"test\":\"linux_mmap_byte_offset\",\"passed\":true}");

    int to_child[2], to_parent[2];
    CHECK(pipe(to_child) == 0 && pipe(to_parent) == 0);
    volatile unsigned char *shared = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    shared[0] = 0;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(to_child[1]); close(to_parent[0]);
        char value;
        for (unsigned i = 1; i <= 64; ++i) {
            CHECK(read(to_child[0], &value, 1) == 1);
            CHECK(shared[0] == i);
            CHECK(pread(fd, data, 1, 0) == 1 && data[0] == 17);
            data[0] = 33;
            CHECK(mprotect(data + PAGE, PAGE, PROT_NONE) == 0);
            CHECK(mprotect(data + PAGE, PAGE, PROT_READ | PROT_WRITE) == 0);
            CHECK(madvise(data + PAGE, PAGE, MADV_DONTNEED) == 0);
            CHECK(data[PAGE] == 0);
            data[PAGE] = 77;
            CHECK(munmap(data + 2 * PAGE, PAGE) == 0);
            CHECK(mmap(data + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == data + 2 * PAGE);
            CHECK(data[2 * PAGE] == 0);
            data[2 * PAGE] = 88;
            shared[0] = (unsigned char)(i + 64);
            CHECK(write(to_parent[1], &value, 1) == 1);
        }
        _exit(0);
    }
    close(to_child[0]); close(to_parent[1]);
    char value = 1;
    for (unsigned i = 1; i <= 64; ++i) {
        shared[0] = (unsigned char)i;
        CHECK(write(to_child[1], &value, 1) == 1);
        CHECK(read(to_parent[0], &value, 1) == 1);
        CHECK(shared[0] == i + 64);
        CHECK(data[0] == 17 && data[PAGE] == 17 && data[2 * PAGE] == 17);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(close(to_child[1]) == 0 && close(to_parent[0]) == 0 && close(fd) == 0);
    CHECK(munmap((void *)shared, PAGE) == 0 && munmap(data, 4 * PAGE) == 0);
    puts("{\"test\":\"linux_user_memory_boundaries\",\"passed\":true,\"reuse_rounds\":64}");
}
