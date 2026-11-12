/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../layout.h"

#define PAGE NCE_PAGE_SIZE
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "aslr-guest:%d errno=%d\n", __LINE__, errno); \
            _exit(93); \
        } \
    } while (0)

static int read_setting(const char *path)
{
    int fd = open(path, O_RDONLY);
    char text[32] = {0};
    int value;
    CHECK(fd >= 0 && read(fd, text, sizeof(text) - 1) > 0 && !close(fd));
    CHECK(sscanf(text, "%d", &value) == 1);
    return value;
}

static int write_setting(const char *path, int value)
{
    int fd = open(path, O_WRONLY);
    char text[32];
    CHECK(fd >= 0);
    int bytes = snprintf(text, sizeof(text), "%d\n", value);
    CHECK(bytes > 0 && bytes < (int)sizeof(text));
    ssize_t result = write(fd, text, bytes);
    int error = errno;
    CHECK(!close(fd));
    if (result < 0) {
        CHECK(error == EINVAL);
        return 0;
    }
    CHECK(result == bytes);
    return 1;
}

static void layout(unsigned long *base, unsigned long *end)
{
    FILE *input = fopen("/probe-layout", "r");
    char name[8];
    CHECK(input && fscanf(input, "%7s %lu %lu", name, base, end) == 3 && !fclose(input));
    CHECK(*base >= 0x10000 && *end > *base);
}

struct sample {
    unsigned long first, second, stack, flags, limit, lower, fallback;
};

static void fill_hole(unsigned long start, unsigned long end)
{
    if (end > start)
        CHECK(mmap((void *)start, end - start, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0) == (void *)start);
}

static void exhaust_topdown(struct sample *sample)
{
    unsigned long base, end, starts[128], ends[128];
    layout(&base, &end);
    FILE *input = fopen("/proc/self/maps", "r");
    CHECK(input);
    char line[512];
    unsigned count = 0;
    while (fgets(line, sizeof(line), input)) {
        CHECK(count < 128 && sscanf(line, "%lx-%lx", &starts[count], &ends[count]) == 2);
        ++count;
    }
    /* Keep the FILE buffer's VMA in the snapshot until allocation checks finish. */
    CHECK(!ferror(input));
    unsigned long ceiling = sample->first + PAGE;
    /* The host layout may register the ELF at the first guest page. */
    unsigned long lowest_free = base;
    for (unsigned i = 0; i < count; ++i) {
        if (starts[i] > lowest_free)
            break;
        if (ends[i] > lowest_free)
            lowest_free = ends[i];
    }
    CHECK(lowest_free < ceiling && lowest_free <= end - PAGE);
    unsigned long cursor = lowest_free + PAGE;
    for (unsigned i = 0; i < count && cursor < ceiling; ++i) {
        if (ends[i] <= cursor)
            continue;
        unsigned long stop = starts[i] < ceiling ? starts[i] : ceiling;
        fill_hole(cursor, stop);
        if (ends[i] > cursor)
            cursor = ends[i];
    }
    fill_hole(cursor, ceiling);
    void *lower = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(lower == (void *)lowest_free);
    *(volatile unsigned long *)lower = 31;
    void *fallback = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(fallback != MAP_FAILED && (unsigned long)fallback >= ceiling && (unsigned long)fallback <= end - PAGE);
    *(volatile unsigned long *)fallback = 37;
    CHECK(*(volatile unsigned long *)lower == 31 && *(volatile unsigned long *)fallback == 37);
    CHECK(mmap((void *)(base - PAGE), PAGE, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED &&
          errno == EACCES);
    sample->lower = (unsigned long)lower;
    sample->fallback = (unsigned long)fallback;
    CHECK(!fclose(input));
}

int translation_aslr_sample(int fd, const char *mode)
{
    struct rlimit limit;
    CHECK(!getrlimit(RLIMIT_STACK, &limit));
    void *first = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *second = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(first != MAP_FAILED && second != MAP_FAILED);
    *(volatile unsigned long *)first = 17;
    *(volatile unsigned long *)second = 23;
    struct sample sample = {(unsigned long)first, (unsigned long)second, (unsigned long)&limit,
                            personality(0xffffffffUL), limit.rlim_cur, 0, 0};
    CHECK(sample.flags != (unsigned long)-1);
    if (!strcmp(mode, "fallback"))
        exhaust_topdown(&sample);
    CHECK(write(fd, &sample, sizeof(sample)) == sizeof(sample) && !close(fd));
    return 0;
}

static struct sample exec_sample(const char *mode, unsigned long flags, unsigned long limit)
{
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(!close(pipefd[0]) && personality(flags) >= 0);
        struct rlimit stack = {limit, RLIM_INFINITY};
        CHECK(!setrlimit(RLIMIT_STACK, &stack));
        char fd[24];
        CHECK(snprintf(fd, sizeof(fd), "%d", pipefd[1]) > 0);
        char *argv[] = {"/probe-guest", "aslr-sample", fd, (char *)mode, NULL};
        extern char **environ;
        execve(argv[0], argv, environ);
        CHECK(0);
    }
    CHECK(!close(pipefd[1]));
    struct sample sample;
    CHECK(read(pipefd[0], &sample, sizeof(sample)) == sizeof(sample) && !close(pipefd[0]));
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    return sample;
}

void translation_aslr_tests(void)
{
    const char *bits_path = "/proc/sys/vm/mmap_rnd_bits";
    const char *legacy_path = "/proc/sys/vm/legacy_va_layout";
    unsigned long base, end;
    layout(&base, &end);
    int minimum = PAGE == 16384 ? 16 : 18;
    int maximum = minimum + (end - base > NCE_USER_SIZE ? 8 : 1);
    unsigned char random[32];
    CHECK(getrandom(random, sizeof(random), GRND_NONBLOCK) == sizeof(random));
    CHECK(read_setting(bits_path) == maximum && read_setting(legacy_path) == 0);
    CHECK(!write_setting(bits_path, minimum - 1) && read_setting(bits_path) == maximum);
    for (int value = 33; value > maximum; --value)
        CHECK(!write_setting(bits_path, value) && read_setting(bits_path) == maximum);
    CHECK(write_setting(bits_path, maximum) && read_setting(bits_path) == maximum);
    printf("{\"config\":\"linux_mmap_aslr\",\"min_bits\":%d,\"max_bits\":%d,\"guest_base\":%lu,\"guest_end\":%lu}\n",
           minimum, maximum, base, end);
    puts("{\"test\":\"linux_mmap_aslr_sysctl_bounds\",\"passed\":true}");

    void *first = mmap(NULL, PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(first != MAP_FAILED && write_setting(bits_path, minimum));
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!close(pipefd[0]));
        void *next = mmap(NULL, PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(next != MAP_FAILED && write(pipefd[1], &next, sizeof(next)) == sizeof(next));
        _exit(0);
    }
    CHECK(!close(pipefd[1]));
    void *child_address;
    CHECK(read(pipefd[0], &child_address, sizeof(child_address)) == sizeof(child_address) && !close(pipefd[0]));
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    void *next = mmap(NULL, PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(next == child_address && (unsigned long)next + PAGE == (unsigned long)first);
    CHECK(!munmap(first, PAGE) && !munmap(next, PAGE));
    puts("{\"test\":\"linux_mmap_aslr_fork_same_mm\",\"passed\":true}");

    const struct {
        const char *name;
        unsigned long flags, limit;
        int bits, legacy, count;
    } cases[] = {
        {"minimum", 0, 8UL << 20, minimum, 0, 8},
        {"maximum", 0, 8UL << 20, maximum, 0, 8},
        {"no_randomize", ADDR_NO_RANDOMIZE, 8UL << 20, maximum, 0, 4},
        {"compat_layout", ADDR_COMPAT_LAYOUT, 8UL << 20, maximum, 0, 4},
        {"infinite_stack", 0, RLIM_INFINITY, maximum, 0, 4},
        {"large_stack", 0, RLIM_INFINITY - 1, maximum, 0, 8},
        {"large_no_randomize", ADDR_NO_RANDOMIZE, RLIM_INFINITY - 1, maximum, 0, 4},
        {"legacy_sysctl", 0, 8UL << 20, maximum, 1, 4},
        {"fallback", ADDR_NO_RANDOMIZE, 8UL << 20, maximum, 0, 1},
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
        CHECK(write_setting(bits_path, cases[c].bits) && write_setting(legacy_path, cases[c].legacy));
        struct sample initial = {0};
        int mmap_varied = 0, stack_varied = 0;
        for (int i = 0; i < cases[c].count; ++i) {
            struct sample sample = exec_sample(cases[c].name, cases[c].flags, cases[c].limit);
            CHECK(sample.first >= base && sample.first <= end - PAGE && !(sample.first % PAGE));
            CHECK(sample.second >= base && sample.second <= end - PAGE && !(sample.second % PAGE));
            CHECK(sample.stack >= base && sample.stack < end && sample.flags == cases[c].flags && sample.limit == cases[c].limit);
            int legacy = cases[c].legacy || (cases[c].flags & ADDR_COMPAT_LAYOUT) || cases[c].limit == RLIM_INFINITY;
            CHECK(legacy ? sample.second == sample.first + PAGE : sample.second + PAGE == sample.first);
            if (!i)
                initial = sample;
            else {
                mmap_varied |= sample.first != initial.first;
                stack_varied |= sample.stack != initial.stack;
            }
            if (cases[c].flags & ADDR_NO_RANDOMIZE)
                CHECK(sample.first == initial.first && sample.stack == initial.stack);
            printf("{\"linux_mmap_aslr_sample\":true,\"case\":\"%s\",\"iteration\":%d,\"bits\":%d,\"first\":%lu,\"second\":%lu,\"stack\":%lu,\"personality\":%lu,\"stack_limit\":%lu,\"lower\":%lu,\"fallback\":%lu}\n",
                   cases[c].name, i, cases[c].bits, sample.first, sample.second, sample.stack,
                   sample.flags, sample.limit, sample.lower, sample.fallback);
        }
        CHECK(cases[c].count == 1 || (cases[c].flags & ADDR_NO_RANDOMIZE) || (mmap_varied && stack_varied));
        printf("{\"test\":\"linux_mmap_aslr\",\"case\":\"%s\",\"passed\":true}\n", cases[c].name);
    }
    CHECK(write_setting(bits_path, maximum) && write_setting(legacy_path, 0));
}
