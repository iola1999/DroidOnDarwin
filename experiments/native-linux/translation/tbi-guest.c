/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "translation-tbi:%d errno=%d\n", __LINE__, errno); \
            _exit(94); \
        } \
    } while (0)
#include "../layout.h"
#define PAGE NCE_PAGE_SIZE
static sigjmp_buf recovery;
static volatile sig_atomic_t delivered;

static void on_fault(int number)
{
    delivered = number;
    siglongjmp(recovery, 1);
}

/* LDTR/STTR are uncovered by Direct and require an actual Unicorn data access. */
static unsigned long fallback_read(unsigned long address)
{
    unsigned long value;
    asm volatile("mov x10,%1\nldtr x9,[x10]\nmov %0,x9" : "=r"(value) : "r"(address) : "x9", "x10", "memory");
    return value;
}
static void fallback_write(unsigned long address, unsigned long value)
{
    asm volatile("mov x10,%0\nmov x9,%1\nsttr x9,[x10]" : : "r"(address), "r"(value) : "x9", "x10", "memory");
}
static void rejected(unsigned long address, int access)
{
    delivered = 0;
    if (!sigsetjmp(recovery, 1)) {
        if (access == 0)
            (void)fallback_read(address);
        else if (access == 1)
            fallback_write(address, 0x71);
        else
            ((void (*)(void))address)();
        CHECK(0);
    }
    CHECK(delivered == SIGSEGV);
}

void translation_tbi_tests(int sparse)
{
    struct sigaction action = {.sa_handler = on_fault}, previous;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV, &action, &previous));
    CHECK(syscall(SYS_prctl, 55, 1, 0, 0, 0) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_prctl, 56, 0, 0, 0, 0) == -1 && errno == EINVAL);
    for (unsigned region = 0; region < (sparse ? 2U : 1U); ++region) {
        unsigned long requested = region ? UINT64_C(0x200040000) : 0;
        unsigned long *page = mmap((void *)requested, 2 * PAGE, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | (region ? MAP_FIXED_NOREPLACE : 0), -1, 0);
        CHECK(page != MAP_FAILED && (!region || (unsigned long)page == requested));
        for (unsigned long tag = 0x02; tag; tag = tag == 0x02 ? 0xb4 : tag == 0xb4 ? 0xff
                                                                                   : 0) {
            unsigned long tagged = (unsigned long)page | (tag << 56);
            page[0] = 19;
            CHECK(*(volatile unsigned long *)tagged == 19 && fallback_read(tagged) == 19);
            *(volatile unsigned long *)tagged = 23;
            CHECK(page[0] == 23);
            fallback_write(tagged, 29);
            CHECK(page[0] == 29);
            unsigned long crossing = tagged + PAGE - 4;
            *(volatile unsigned long *)crossing = UINT64_C(0x13579abcdef);
            CHECK(*(volatile unsigned long *)crossing == UINT64_C(0x13579abcdef));
            CHECK(fallback_read(crossing) == UINT64_C(0x13579abcdef));
            CHECK(!mprotect(page, PAGE, PROT_READ));
            rejected(tagged, 1);
            CHECK(page[0] == 29 && fallback_read(tagged) == 29);
            CHECK(!mprotect(page, PAGE, PROT_READ | PROT_WRITE));
            fallback_write(tagged, 31);
            CHECK(page[0] == 31);
            CHECK(!munmap(page, PAGE));
            rejected(tagged, 0);
            CHECK(mmap(page, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == page);
            page[0] = 41;
            CHECK(fallback_read(tagged) == 41);
            rejected(tagged | (1UL << 48), 0);
            rejected(tagged | (1UL << 55), 1);
            CHECK(page[0] == 41);
            strcpy((char *)page, "/host-pointer");
            int fd = open("/host-pointer", O_RDWR);
            CHECK(fd >= 0);
            CHECK(syscall(SYS_write, fd, tagged, 1) == -1 && errno == EFAULT);
            CHECK(syscall(SYS_pread64, fd, tagged, 1, 0) == -1 && errno == EFAULT);
            CHECK(syscall(SYS_openat, AT_FDCWD, tagged, O_RDONLY, 0) == -1 && errno == EFAULT);
            CHECK(!strcmp((char *)page, "/host-pointer") && !close(fd));
        }
        CHECK(!munmap(page, 2 * PAGE));
    }
    unsigned long tagged_code = (unsigned long)translation_tbi_tests | UINT64_C(0x0200000000000000);
    CHECK(fallback_read(tagged_code) == *(const unsigned long *)translation_tbi_tests);
    rejected(tagged_code, 2);
    CHECK(!sigaction(SIGSEGV, &previous, NULL));
    puts("{\"test\":\"translated_data_tbi_and_unicorn_alias_invalidation\",\"passed\":true}");
    puts("{\"test\":\"translated_tagged_syscall_abi_rejected\",\"passed\":true}");
    puts("{\"test\":\"translated_tagged_fetch_rejected\",\"passed\":true}");
}
