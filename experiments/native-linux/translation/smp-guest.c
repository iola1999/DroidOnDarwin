/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "../layout.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "parallel guest:%d: %s\n", __LINE__, #condition); \
            exit(1); \
        } \
    } while (0)

static uint64_t now(void)
{
    struct timespec value;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &value));
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

struct Shared {
    unsigned ready[2];
    uint64_t start[2], end[2];
};

static void compute(struct Shared *shared, volatile uint64_t *private, unsigned index)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(index, &set);
    CHECK(!sched_setaffinity(0, sizeof(set), &set));
    CHECK(sched_getcpu() == (int)index);
    *private = index + 11;
    __atomic_store_n(&shared->ready[index], 1, __ATOMIC_RELEASE);
    uint64_t limit = now() + UINT64_C(10000000000);
    while (!__atomic_load_n(&shared->ready[index ^ 1], __ATOMIC_ACQUIRE)) {
        CHECK(now() < limit);
        sched_yield();
    }
    shared->start[index] = now();
    uint64_t expected = index + 11;
    for (unsigned iteration = 0; iteration < 4000000; ++iteration) {
        expected = expected * 1664525 + 1013904223;
        *private = *private * 1664525 + 1013904223;
    }
    shared->end[index] = now();
    CHECK(*private == expected);
}

static void wait_for(unsigned *word, unsigned value)
{
    uint64_t limit = now() + UINT64_C(10000000000);
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != value) {
        CHECK(now() < limit);
        sched_yield();
    }
}

static void shared_code_test(struct Shared *shared)
{
    int fd = memfd_create("parallel-code", MFD_CLOEXEC);
    CHECK(fd >= 0 && !ftruncate(fd, 16384));
    volatile uint32_t *rw = mmap(NULL, 16384, 3, MAP_SHARED, fd, 0);
    CHECK(rw != MAP_FAILED);
    // TPIDRRO forces the fallback path; its value is deliberately unused.
    rw[0] = 0xd53bd061;
    rw[1] = 0x52800000;
    rw[2] = 0xd65f03c0;
    uint64_t (*rx)(void) = (void *)syscall(NCE_SYS_MAP_CODE, fd, 16384, 0);
    CHECK((void *)rx != MAP_FAILED && rx() == 0);
    CHECK(getpid() > 0);
    shared->ready[0] = shared->ready[1] = 0;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(1, &set);
        CHECK(!sched_setaffinity(0, sizeof(set), &set));
        int pipefd[2];
        CHECK(!pipe(pipefd));
        for (unsigned iteration = 1; iteration <= 128; ++iteration) {
            wait_for(&shared->ready[0], iteration - 1);
            uint32_t word = 0x52800000 | (iteration << 5);
            if (iteration & 1)
                rw[1] = word;
            else {
                CHECK(write(pipefd[1], &word, sizeof(word)) == sizeof(word));
                CHECK(read(pipefd[0], (void *)(rw + 1), sizeof(word)) == sizeof(word));
            }
            __atomic_store_n(&shared->ready[1], iteration, __ATOMIC_RELEASE);
        }
        CHECK(!close(pipefd[0]) && !close(pipefd[1]));
        _exit(0);
    }
    for (unsigned iteration = 1; iteration <= 128; ++iteration) {
        wait_for(&shared->ready[1], iteration);
        uint64_t value = rx();
        if (value != iteration)
            fprintf(stderr, "shared code: iteration=%u returned=%llu\n", iteration, (unsigned long long)value);
        CHECK(value == iteration);
        CHECK(getpid() > 0);
        __atomic_store_n(&shared->ready[0], iteration, __ATOMIC_RELEASE);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    CHECK(!munmap((void *)rx, 16384) && !munmap((void *)rw, 16384) && !close(fd));
    puts("{\"test\":\"translated_smp_code_alias_updates\",\"passed\":true,\"updates\":128}");
}

struct CopyRace {
    unsigned char *address;
    unsigned ready, done;
};

static void *replace_copy_source(void *argument)
{
    struct CopyRace *race = argument;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(1, &set);
    CHECK(!sched_setaffinity(0, sizeof(set), &set));
    wait_for(&race->ready, 1);
    for (unsigned pass = 0; pass < 1000; ++pass) {
        CHECK(!munmap(race->address, 16384));
        CHECK(mmap(race->address, 16384, 3, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == race->address);
        for (unsigned byte = 0; byte < 64; ++byte)
            race->address[byte] = 0x5a;
        CHECK(!mprotect(race->address, 16384, PROT_NONE));
        CHECK(!mprotect(race->address, 16384, PROT_READ | PROT_WRITE));
        sched_yield();
    }
    __atomic_store_n(&race->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void kernel_copy_race(void)
{
    struct CopyRace race = {0};
    race.address = mmap(NULL, 16384, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(race.address != MAP_FAILED);
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pthread_t worker;
    CHECK(!pthread_create(&worker, NULL, replace_copy_source, &race));
    __atomic_store_n(&race.ready, 1, __ATOMIC_RELEASE);
    unsigned copies = 0, faults = 0, passes = 0;
    do {
        ssize_t result = write(pipefd[1], race.address, 64);
        if (result < 0) {
            CHECK(errno == EFAULT);
            ++faults;
        } else {
            CHECK(result == 64);
            unsigned char buffer[64];
            CHECK(read(pipefd[0], buffer, sizeof(buffer)) == sizeof(buffer));
            // A copy may overlap initialization; every byte is from valid RAM.
            for (unsigned byte = 0; byte < sizeof(buffer); ++byte)
                CHECK(buffer[byte] == 0 || buffer[byte] == 0x5a);
            ++copies;
        }
        ++passes;
    } while (passes < 1000 || !__atomic_load_n(&race.done, __ATOMIC_ACQUIRE));
    CHECK(!pthread_join(worker, NULL) && copies);
    CHECK(!close(pipefd[0]) && !close(pipefd[1]) && !munmap(race.address, 16384));
    printf("{\"test\":\"translated_smp_copy_unmap_protect\",\"passed\":true,\"copies\":%u,\"faults\":%u}\n", copies, faults);
}

void translation_smp_guest_test(void)
{
    struct Shared *shared = mmap(NULL, 16384, 3, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    volatile uint64_t *private = mmap(NULL, 16384, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED && private != MAP_FAILED);
    *private = 7;
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        compute(shared, private, 1);
        _exit(0);
    }
    compute(shared, private, 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    uint64_t begin = shared->start[0] > shared->start[1] ? shared->start[0] : shared->start[1];
    uint64_t end = shared->end[0] < shared->end[1] ? shared->end[0] : shared->end[1];
    CHECK(end > begin);
    printf("{\"test\":\"translated_smp_private_mm_compute\",\"passed\":true,\"iterations_per_cpu\":4000000,\"cpu0_ns\":%llu,\"cpu1_ns\":%llu,\"interval_overlap_ns\":%llu}\n",
           (unsigned long long)(shared->end[0] - shared->start[0]),
           (unsigned long long)(shared->end[1] - shared->start[1]), (unsigned long long)(end - begin));
    shared_code_test(shared);
    kernel_copy_race();
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    CPU_SET(1, &set);
    CHECK(!sched_setaffinity(0, sizeof(set), &set));
    CHECK(!munmap(shared, 16384) && !munmap((void *)private, 16384));
}
