/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../layout.h"
#define PAGE NCE_PAGE_SIZE
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "sparse-guest:%d errno=%d\n", __LINE__, errno); \
            _exit(94); \
        } \
    } while (0)

void translation_sparse_tests(void (*inaccessible)(unsigned long, int))
{
    FILE *input = fopen("/probe-host-sources", "r");
    unsigned long host[2];
    CHECK(input && fscanf(input, "%lu %lu", &host[0], &host[1]) == 2 && !fclose(input));
    CHECK(host[0] >= 0x10000 && host[0] < NCE_SPARSE_GUEST_END && host[1] >= NCE_SPARSE_GUEST_END);
    int file = open("/host-pointer", O_RDWR);
    CHECK(file >= 0);
    for (unsigned i = 0; i < 2; ++i) {
        inaccessible(host[i], 0);
        inaccessible(host[i], 1);
        CHECK(syscall(SYS_openat, AT_FDCWD, host[i], O_RDONLY, 0) == -1 && errno == EFAULT);
        CHECK(syscall(SYS_write, file, host[i] + 128, 8) == -1 && errno == EFAULT);
        CHECK(syscall(SYS_pread64, file, host[i] + 256, 8, 0) == -1 && errno == EFAULT);
    }
    /* A guest mapping can use the numeric address of a host buffer. Every
     * syscall must then copy through this mm's PTE and leave the host intact. */
    char *same = mmap((void *)host[0], PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(same == (void *)host[0]);
    strcpy(same, "/guest-source");
    int guest_file = open(same, O_CREAT | O_RDWR, 0600);
    CHECK(guest_file >= 0);
    strcpy(same + 128, "guest-pte");
    CHECK(write(guest_file, same + 128, 10) == 10);
    CHECK(pread(guest_file, same + 256, 10, 0) == 10 && !strcmp(same + 256, "guest-pte"));
    CHECK(!close(guest_file));
    pid_t same_child = fork();
    CHECK(same_child >= 0);
    if (!same_child) {
        CHECK(!strcmp(same + 128, "guest-pte"));
        strcpy(same + 128, "child-cow");
        CHECK(!strcmp(same + 128, "child-cow"));
        _exit(0);
    }
    int same_status;
    CHECK(waitpid(same_child, &same_status, 0) == same_child && WIFEXITED(same_status) && !WEXITSTATUS(same_status));
    CHECK(!strcmp(same + 128, "guest-pte") && !munmap(same, PAGE));
    int same_fd = syscall(SYS_memfd_create, "same-host-address", 0);
    CHECK(same_fd >= 0 && !ftruncate(same_fd, PAGE));
    same = mmap((void *)host[0], PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, same_fd, 0);
    CHECK(same == (void *)host[0]);
    strcpy(same + 128, "shared-before");
    same_child = fork();
    CHECK(same_child >= 0);
    if (!same_child) {
        CHECK(!strcmp(same + 128, "shared-before"));
        strcpy(same + 128, "shared-after");
        _exit(0);
    }
    CHECK(waitpid(same_child, &same_status, 0) == same_child && WIFEXITED(same_status) && !WEXITSTATUS(same_status));
    CHECK(!strcmp(same + 128, "shared-after"));
    CHECK(!munmap(same, PAGE) && !close(same_fd));
    inaccessible(host[0], 0);
    inaccessible(host[0], 1);
    puts("{\"test\":\"translated_sparse_host_pointer_sources\",\"passed\":true}");
    puts("{\"test\":\"translated_same_host_va_cow_shared\",\"passed\":true}");

    volatile unsigned long *low = mmap((void *)(0x10000 + NCE_MMAP_OFFSET), PAGE, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(low == (void *)(0x10000 + NCE_MMAP_OFFSET));
    unsigned long high_address = (unsigned long)low + (1UL << 32);
    volatile unsigned long *high = mmap((void *)high_address, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(high == (void *)high_address);
    *low = 17;
    *high = 29;
    CHECK(*low == 17 && *high == 29);
    unsigned long value = 41;
    CHECK(pwrite(file, &value, 8, 0) == 8 && pread(file, (void *)high, 8, 0) == 8 && *high == 41);
    CHECK(pwrite(file, (void *)high, 8, 0) == 8);
    CHECK(!mprotect((void *)high, PAGE, PROT_READ));
    inaccessible(high_address, 1);
    CHECK(*high == 41 && *low == 17);
    CHECK(!mprotect((void *)high, PAGE, PROT_READ | PROT_WRITE));
    volatile unsigned long *cross = (void *)(high_address + PAGE - 4);
    *cross = UINT64_C(0x123456789abcdef0);
    CHECK(*cross == UINT64_C(0x123456789abcdef0));
    unsigned long original = __atomic_fetch_add((unsigned long *)high, 1, __ATOMIC_SEQ_CST);
    CHECK(original == 41 && *high == 42);

    int memory = syscall(SYS_memfd_create, "sparse-shared", 0);
    CHECK(memory >= 0 && !ftruncate(memory, PAGE));
    volatile unsigned long *shared = mmap((void *)(32UL << 30), PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, memory, 0);
    CHECK(shared == (void *)(32UL << 30));
    *shared = 53;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(*high == 42 && *low == 17 && *shared == 53);
        *high = 61;
        *shared = 71;
        CHECK(*high == 61 && *low == 17);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(*high == 42 && *low == 17 && *shared == 71);
    CHECK(!munmap((void *)shared, PAGE) && !close(memory));
    CHECK(!munmap((void *)high, 2 * PAGE));
    inaccessible(high_address, 0);
    inaccessible(high_address, 1);
    CHECK(*low == 17 && !munmap((void *)low, PAGE));
    CHECK(!close(file));
    puts("{\"test\":\"translated_sparse_pte_cow_shared_uaccess\",\"passed\":true}");

    const unsigned long reservation_size = 9UL << 30;
    void *reservation = mmap(NULL, reservation_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(reservation != MAP_FAILED);
    CHECK((unsigned long)reservation >= 0x10000 && (unsigned long)reservation <= NCE_SPARSE_GUEST_END - reservation_size);
    volatile unsigned long *last = reservation + reservation_size - PAGE;
    CHECK(!mprotect((void *)last, PAGE, PROT_READ | PROT_WRITE));
    *last = 83;
    CHECK(*last == 83);
    CHECK(!munmap(reservation, reservation_size));
    inaccessible((unsigned long)last, 0);
    puts("{\"test\":\"translated_sparse_reservation_materialization\",\"passed\":true}");
}
