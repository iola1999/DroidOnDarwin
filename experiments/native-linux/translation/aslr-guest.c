/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../layout.h"
#include "exec-aslr.h"

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
    int legacy = read_setting("/proc/sys/vm/legacy_va_layout") ||
                 (sample.flags & ADDR_COMPAT_LAYOUT) || sample.limit == RLIM_INFINITY;
    CHECK(legacy ? sample.second >= sample.first + PAGE : sample.second + PAGE <= sample.first);
    /* ELF alignment can leave a one-page gap above an occupied VMA. */
    exec_aslr_check_mapped(legacy ? sample.first + PAGE : sample.second + PAGE,
                           legacy ? sample.second : sample.first);
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

static void read_exact(int fd, void *output, size_t length)
{
    char *cursor = output;
    while (length) {
        ssize_t bytes = read(fd, cursor, length);
        CHECK(bytes > 0);
        cursor += bytes;
        length -= bytes;
    }
}

static void image_exec(const char *fixture, int fd)
{
    char path[64], number[24];
    CHECK(snprintf(path, sizeof(path), "/exec-aslr-%s", !strcmp(fixture, "loader") ? "pie" : fixture) > 0);
    CHECK(snprintf(number, sizeof(number), "%d", fd) > 0);
    char *args[] = {path, "exec-aslr-sample", number, NULL};
    char *loader[] = {"/lib/ld-musl-aarch64.so.1", path, "exec-aslr-sample", number, NULL};
    extern char **environ;
    execve(!strcmp(fixture, "loader") ? loader[0] : path,
           !strcmp(fixture, "loader") ? loader : args, environ);
    CHECK(0);
}

static struct exec_aslr_sample image_sample(const char *fixture, unsigned long flags)
{
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!close(pipefd[0]) && personality(flags) >= 0);
        image_exec(fixture, pipefd[1]);
    }
    CHECK(!close(pipefd[1]));
    struct exec_aslr_sample sample;
    read_exact(pipefd[0], &sample, sizeof(sample));
    CHECK(!close(pipefd[0]));
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    return sample;
}

static size_t format_image(char *text, size_t capacity, const char *name, const struct exec_aslr_image *image)
{
    int bytes = snprintf(text, capacity, ",\"%s\":{\"bias\":%lu,\"first_vaddr\":%lu,\"type\":%u,\"loads\":[",
                         name, image->bias, image->first_vaddr, image->type);
    CHECK(bytes > 0 && (size_t)bytes < capacity);
    size_t used = bytes;
    for (unsigned i = 0; i < image->count; ++i) {
        const struct exec_aslr_segment *load = &image->loads[i];
        bytes = snprintf(text + used, capacity - used,
                         "%s{\"address\":%lu,\"offset\":%lu,\"filesz\":%lu,\"memsz\":%lu,\"align\":%lu,\"flags\":%u}",
                         i ? "," : "", load->address, load->offset, load->filesz, load->memsz, load->align, load->flags);
        CHECK(bytes > 0 && (size_t)bytes < capacity - used);
        used += bytes;
    }
    CHECK(used + 2 < capacity);
    text[used++] = ']';
    text[used++] = '}';
    text[used] = 0;
    return used;
}

static unsigned long image_end(const struct exec_aslr_image *image)
{
    unsigned long end = 0;
    for (unsigned i = 0; i < image->count; ++i) {
        const struct exec_aslr_segment *load = &image->loads[i];
        unsigned long next = (load->address + load->memsz + PAGE - 1) & ~(PAGE - 1);
        if (next > end)
            end = next;
    }
    return end;
}

static void exec_randomization_tests(unsigned long base, unsigned long end)
{
    const char *random_path = "/proc/sys/kernel/randomize_va_space";
    const char *fixtures[] = {"static", "static-2m", "pie", "pie-256k", "pie-2m", "pie-nonzero", "exec", "exec-near-end", "loader"};
    const struct {
        const char *name;
        int randomize;
        unsigned long flags;
    } modes[] = {{"off", 0, 0}, {"mmap_only", 1, 0}, {"full", 2, 0}, {"personality", 2, ADDR_NO_RANDOMIZE}};
    printf("{\"config\":\"linux_exec_aslr\",\"elf_base\":%lu,\"brk_range_bytes\":%lu}\n",
           base + NCE_MMAP_OFFSET, 1UL << 30);
    for (unsigned m = 0; m < sizeof(modes) / sizeof(*modes); ++m) {
        CHECK(write_setting(random_path, modes[m].randomize));
        for (unsigned f = 0; f < sizeof(fixtures) / sizeof(*fixtures); ++f) {
            /* This ET_EXEC fixture is linked at 16 MiB for low/sparse guests. */
            if (!strcmp(fixtures[f], "exec") && base > 0x1000000UL)
                continue;
            if (!strcmp(fixtures[f], "exec-near-end") && end != NCE_SPARSE_GUEST_END)
                continue;
            struct exec_aslr_sample initial = {0};
            int program_varied = 0, interpreter_varied = 0, brk_varied = 0, sigreturn_varied = 0;
            for (unsigned i = 0; i < 4; ++i) {
                struct exec_aslr_sample sample = image_sample(fixtures[f], modes[m].flags);
                char text[8192];
                int bytes = snprintf(text, sizeof(text),
                                     "{\"linux_exec_aslr_sample\":true,\"fixture\":\"%s\",\"mode\":\"%s\",\"iteration\":%u,"
                                     "\"phdr\":%lu,\"entry\":%lu,\"at_base\":%lu,\"start_brk\":%lu,\"brk\":%lu,\"stack\":%lu,\"sigreturn\":%lu,\"personality\":%lu",
                                     fixtures[f], modes[m].name, i, sample.phdr, sample.entry, sample.at_base,
                                     sample.start_brk, sample.brk, sample.stack, sample.sigreturn, sample.personality);
                CHECK(bytes > 0 && (size_t)bytes < sizeof(text));
                size_t used = bytes;
                used += format_image(text + used, sizeof(text) - used, "program", &sample.program);
                used += format_image(text + used, sizeof(text) - used, "interpreter", &sample.interpreter);
                CHECK(used + 2 <= sizeof(text));
                text[used++] = '}';
                text[used++] = '\n';
                CHECK(write(STDOUT_FILENO, text, used) == (ssize_t)used);
                CHECK(sample.personality == modes[m].flags && sample.brk >= base && sample.brk <= end);
                int randomized = modes[m].randomize && !modes[m].flags;
                unsigned long brk_base;
                int relocated_brk = sample.program.type == ET_DYN &&
                                    (!sample.interpreter.count || !strcmp(fixtures[f], "loader"));
                if (randomized && modes[m].randomize == 2 && relocated_brk)
                    brk_base = base + NCE_MMAP_OFFSET;
                else
                    brk_base = image_end(!strcmp(fixtures[f], "loader") ? &sample.interpreter : &sample.program);
                if (randomized && modes[m].randomize == 2) {
                    if (!relocated_brk)
                        brk_base += PAGE;
                    CHECK(sample.start_brk >= brk_base && sample.start_brk - brk_base < (1UL << 30));
                } else
                    CHECK(sample.start_brk == brk_base);
                if (!i)
                    initial = sample;
                else {
                    program_varied |= sample.program.bias != initial.program.bias;
                    interpreter_varied |= sample.interpreter.bias != initial.interpreter.bias;
                    brk_varied |= sample.brk != initial.brk;
                    sigreturn_varied |= sample.sigreturn != initial.sigreturn;
                }
                if (!randomized)
                    CHECK(!program_varied && !interpreter_varied && !brk_varied && !sigreturn_varied);
            }
            if (modes[m].randomize && !modes[m].flags) {
                CHECK(initial.program.type != ET_DYN || program_varied);
                CHECK(!initial.interpreter.count || interpreter_varied);
                CHECK(sigreturn_varied);
                CHECK(initial.program.type != ET_EXEC || modes[m].randomize != 2 || brk_varied);
            }
            printf("{\"test\":\"linux_exec_aslr\",\"fixture\":\"%s\",\"mode\":\"%s\",\"passed\":true}\n",
                   fixtures[f], modes[m].name);
        }
    }
    CHECK(write_setting(random_path, 2));
}

static volatile unsigned long elf_private = 19;

static void exec_cow_tests(void)
{
    struct exec_aslr_sample parent;
    exec_aslr_snapshot("/probe-guest", &parent);
    unsigned long original_brk = syscall(SYS_brk, 0);
    unsigned long heap_address = (original_brk + PAGE - 1) & ~(PAGE - 1);
    CHECK((unsigned long)syscall(SYS_brk, heap_address + PAGE) == heap_address + PAGE);
    volatile unsigned long *heap = (void *)heap_address;
    *heap = 23;
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!close(pipefd[0]) && elf_private == 19 && *heap == 23);
        CHECK((unsigned long)syscall(SYS_brk, 0) == heap_address + PAGE);
        CHECK(exec_aslr_sigreturn() == parent.sigreturn);
        elf_private = 31;
        *heap = 37;
        struct exec_aslr_sample inherited;
        exec_aslr_snapshot("/probe-guest", &inherited);
        CHECK(inherited.phdr == parent.phdr && inherited.entry == parent.entry && inherited.sigreturn == parent.sigreturn);
        CHECK(elf_private == 31 && *heap == 37);
        image_exec("static", pipefd[1]);
    }
    CHECK(!close(pipefd[1]));
    struct exec_aslr_sample executed;
    read_exact(pipefd[0], &executed, sizeof(executed));
    CHECK(!close(pipefd[0]));
    int status;
    CHECK(waitpid(child, &status, 0) == child && !status);
    CHECK(elf_private == 19 && *heap == 23 && exec_aslr_sigreturn() == parent.sigreturn);
    unsigned long heap_value = *heap;
    CHECK((unsigned long)syscall(SYS_brk, 0) == heap_address + PAGE);
    CHECK((unsigned long)syscall(SYS_brk, original_brk) == original_brk);
    CHECK(executed.sigreturn != parent.sigreturn && executed.phdr != parent.phdr);
    printf(
        "{\"test\":\"linux_exec_aslr_fork_cow\",\"passed\":true,\"parent_phdr\":%lu,\"child_phdr\":%lu,"
        "\"parent_sigreturn\":%lu,\"child_sigreturn\":%lu,\"heap\":%lu,\"elf_private\":%lu,\"heap_private\":%lu}\n",
        parent.phdr, executed.phdr, parent.sigreturn, executed.sigreturn, heap_address, elf_private, heap_value);
}

static int copy_elf(const char *source, const char *path)
{
    int input = open(source, O_RDONLY), output = open(path, O_CREAT | O_TRUNC | O_RDWR, 0755);
    CHECK(input >= 0 && output >= 0);
    char buffer[PAGE];
    ssize_t bytes;
    while ((bytes = read(input, buffer, sizeof(buffer))) > 0)
        CHECK(write(output, buffer, bytes) == bytes);
    CHECK(!bytes && !close(input));
    return output;
}

static void rejected_exec(const char *mode, unsigned long base, unsigned long end)
{
    int interp_align = !strcmp(mode, "interp_align_below_guest");
    const char *source = interp_align ? "/lib/ld-musl-aarch64.so.1" : !strcmp(mode, "align_below_guest")      ? "/exec-aslr-pie"
                                                                  : !strcmp(mode, "static_align_below_guest") ? "/exec-aslr-static"
                                                                                                              : "/exec-aslr-exec";
    int output = copy_elf(source, interp_align ? "/exec-aslr-reject-loader" : "/exec-aslr-reject");
    Elf64_Ehdr header;
    Elf64_Phdr phdrs[32];
    CHECK(pread(output, &header, sizeof(header), 0) == sizeof(header));
    CHECK(header.e_phnum && header.e_phnum <= 32);
    size_t length = header.e_phnum * sizeof(*phdrs);
    CHECK(pread(output, phdrs, length, header.e_phoff) == (ssize_t)length);
    unsigned first = 32, last = 32;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (first == 32)
                first = i;
            last = i;
        }
    }
    CHECK(first != 32 && last != 32);
    if (interp_align || !strcmp(mode, "align_below_guest") || !strcmp(mode, "static_align_below_guest"))
        phdrs[first].p_align = 1UL << 47;
    else {
        unsigned long target = !strcmp(mode, "below_guest") ? base - PAGE : !strcmp(mode, "brk_above_guest") ? base + NCE_MMAP_OFFSET
                                                                                                             : end - PAGE;
        unsigned long delta = target - (phdrs[first].p_vaddr & ~(PAGE - 1));
        for (unsigned i = 0; i < header.e_phnum; ++i)
            if (phdrs[i].p_type == PT_LOAD)
                phdrs[i].p_vaddr += delta;
        header.e_entry += delta;
        if (!strcmp(mode, "brk_above_guest"))
            phdrs[last].p_memsz = end - phdrs[last].p_vaddr + PAGE;
    }
    CHECK(pwrite(output, &header, sizeof(header), 0) == sizeof(header));
    CHECK(pwrite(output, phdrs, length, header.e_phoff) == (ssize_t)length && !close(output));
    if (interp_align) {
        output = copy_elf("/exec-aslr-pie", "/exec-aslr-reject");
        CHECK(pread(output, &header, sizeof(header), 0) == sizeof(header));
        CHECK(header.e_phnum && header.e_phnum <= 32);
        length = header.e_phnum * sizeof(*phdrs);
        CHECK(pread(output, phdrs, length, header.e_phoff) == (ssize_t)length);
        unsigned matches = 0;
        for (unsigned i = 0; i < header.e_phnum; ++i) {
            if (phdrs[i].p_type != PT_INTERP)
                continue;
            char path[64] = "/exec-aslr-reject-loader";
            CHECK(phdrs[i].p_filesz > strlen(path) && phdrs[i].p_filesz <= sizeof(path));
            CHECK(pwrite(output, path, phdrs[i].p_filesz, phdrs[i].p_offset) == (ssize_t)phdrs[i].p_filesz);
            ++matches;
        }
        CHECK(matches == 1 && !close(output));
    }
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!close(pipefd[0]));
        char *args[] = {"/exec-aslr-reject", NULL};
        execv(args[0], args);
        int error = errno;
        CHECK(write(pipefd[1], &error, sizeof(error)) == sizeof(error));
        _exit(0);
    }
    CHECK(!close(pipefd[1]));
    int error = 0, status;
    ssize_t bytes = read(pipefd[0], &error, sizeof(error));
    CHECK((bytes == 0 || bytes == sizeof(error)) && !close(pipefd[0]));
    CHECK(waitpid(child, &status, 0) == child);
    CHECK((WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV && !error) ||
          (!status && (error == ENOEXEC || error == EACCES || error == ENOMEM || error == EINVAL)));
    CHECK(!unlink("/exec-aslr-reject") && (!interp_align || !unlink("/exec-aslr-reject-loader")));
    printf("{\"test\":\"linux_exec_aslr_rejected_range\",\"mode\":\"%s\",\"passed\":true,\"status\":%d,\"errno\":%d}\n",
           mode, status, error);
}

static void exec_range_tests(unsigned long base, unsigned long end)
{
    const unsigned long boundaries[] = {base, end - PAGE};
    for (unsigned i = 0; i < 2; ++i) {
        volatile unsigned long *page = mmap((void *)boundaries[i], PAGE, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        CHECK((unsigned long)page == boundaries[i]);
        *page = 41 + i;
        CHECK(*page == 41 + i && !munmap((void *)page, PAGE));
    }
    void *hint = mmap((void *)(base - PAGE), PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(hint != MAP_FAILED && (unsigned long)hint >= base && (unsigned long)hint <= end - PAGE);
    CHECK(!munmap(hint, PAGE));
    CHECK(mmap((void *)(base - PAGE), PAGE, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED &&
          errno == EACCES);
    CHECK(mmap((void *)(end - PAGE), 2 * PAGE, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED &&
          errno == EACCES);
    CHECK(mmap(NULL, end - base + PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOMEM);
    unsigned long brk = syscall(SYS_brk, 0);
    CHECK((unsigned long)syscall(SYS_brk, ~0UL - PAGE + 1) == brk);
    CHECK((unsigned long)syscall(SYS_brk, end + 1) == brk);
    puts("{\"test\":\"linux_exec_aslr_syscall_range\",\"passed\":true}");
    const char *modes[] = {"below_guest", "above_guest", "brk_above_guest", "align_below_guest", "static_align_below_guest", "interp_align_below_guest"};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); ++i)
        rejected_exec(modes[i], base, end);
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
    CHECK(next != MAP_FAILED && next == child_address && (unsigned long)next + PAGE <= (unsigned long)first);
    exec_aslr_check_mapped((unsigned long)next + PAGE, (unsigned long)first);
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
            CHECK(legacy ? sample.second >= sample.first + PAGE : sample.second + PAGE <= sample.first);
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
    exec_randomization_tests(base, end);
    exec_cow_tests();
    exec_range_tests(base, end);
}
