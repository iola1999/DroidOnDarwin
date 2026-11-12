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
#include <sys/auxv.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "../layout.h"
#include "exec-aslr.h"

#define PAGE NCE_PAGE_SIZE
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "exec-aslr-image:%d errno=%d\n", __LINE__, errno); \
            _exit(94); \
        } \
    } while (0)

struct mapping {
    unsigned long start, end, offset;
    char permissions[5], path[128];
};

static volatile unsigned long return_address;
static volatile sig_atomic_t returned;

__attribute__((noinline)) static void restorer(int signal)
{
    CHECK(signal == SIGUSR1);
    return_address = (unsigned long)__builtin_return_address(0);
    ++returned;
}

unsigned long exec_aslr_sigreturn(void)
{
    struct kernel_sigaction {
        unsigned long handler, flags, restorer, mask;
    } original, action = {.handler = (unsigned long)restorer};
    CHECK(!syscall(SYS_rt_sigaction, SIGUSR1, NULL, &original, sizeof(original.mask)));
    CHECK(!syscall(SYS_rt_sigaction, SIGUSR1, &action, NULL, sizeof(action.mask)));
    sig_atomic_t before = returned;
    CHECK(!kill(getpid(), SIGUSR1) && returned == before + 1);
    CHECK(!syscall(SYS_rt_sigaction, SIGUSR1, &original, NULL, sizeof(original.mask)));
    const Elf64_Ehdr *header = (void *)(return_address & ~(PAGE - 1));
    CHECK(!memcmp(header->e_ident, "\177ELF\2\1\1", 7));
    CHECK((unsigned long)header + header->e_entry == return_address);
    return return_address;
}

static unsigned read_maps(struct mapping *maps, unsigned capacity)
{
    int fd = open("/proc/self/maps", O_RDONLY);
    CHECK(fd >= 0);
    char text[16384] = {0};
    size_t used = 0;
    ssize_t bytes;
    while ((bytes = read(fd, text + used, sizeof(text) - used - 1)) > 0) {
        used += bytes;
        CHECK(used < sizeof(text) - 1);
    }
    CHECK(!bytes && !close(fd));
    unsigned count = 0;
    char *save, *line = strtok_r(text, "\n", &save);
    while (line) {
        CHECK(count < capacity);
        struct mapping *map = &maps[count++];
        memset(map, 0, sizeof(*map));
        CHECK(sscanf(line, "%lx-%lx %4s %lx %*s %*s %127s",
                     &map->start, &map->end, map->permissions, &map->offset, map->path) >= 4);
        CHECK(map->start < map->end && !(map->start % PAGE) && !(map->end % PAGE));
        line = strtok_r(NULL, "\n", &save);
    }
    return count;
}

void exec_aslr_check_mapped(unsigned long start, unsigned long end)
{
    if (start == end)
        return;
    CHECK(start < end);
    struct mapping maps[128];
    unsigned count = read_maps(maps, 128);
    unsigned long cursor = start;
    for (unsigned i = 0; i < count && cursor < end; ++i) {
        if (maps[i].end <= cursor)
            continue;
        CHECK(maps[i].start <= cursor);
        cursor = maps[i].end;
    }
    CHECK(cursor >= end);
}

static void check_load(const struct mapping *maps, unsigned count, const char *path,
                       const struct exec_aslr_segment *load, unsigned long base, unsigned long end)
{
    CHECK(load->address >= base && load->address < end && load->memsz <= end - load->address);
    CHECK(load->filesz <= load->memsz && (load->address % PAGE) == (load->offset % PAGE));
    unsigned long cursor = load->address & ~(PAGE - 1);
    unsigned long file_end = (load->address + load->filesz + PAGE - 1) & ~(PAGE - 1);
    unsigned long load_end = (load->address + load->memsz + PAGE - 1) & ~(PAGE - 1);
    while (cursor < load_end) {
        unsigned i;
        for (i = 0; i < count; ++i)
            if (maps[i].start <= cursor && cursor < maps[i].end)
                break;
        CHECK(i < count);
        const struct mapping *map = &maps[i];
        CHECK(!(load->flags & PF_R) || map->permissions[0] == 'r');
        CHECK(!(load->flags & PF_X) || map->permissions[2] == 'x');
        if (load->filesz && cursor < file_end) {
            CHECK(!strcmp(map->path, path));
            CHECK(map->offset + cursor - map->start ==
                  (load->offset & ~(PAGE - 1)) + cursor - (load->address & ~(PAGE - 1)));
        }
        cursor = map->end < load_end ? map->end : load_end;
    }
}

static void read_image(const char *path, unsigned long phdr_address,
                       const struct mapping *maps, unsigned map_count,
                       unsigned long base, unsigned long end, struct exec_aslr_image *image,
                       char *interpreter)
{
    int fd = open(path, O_RDONLY);
    Elf64_Ehdr header;
    Elf64_Phdr phdrs[32];
    CHECK(fd >= 0 && pread(fd, &header, sizeof(header), 0) == sizeof(header));
    CHECK(!memcmp(header.e_ident, "\177ELF\2\1\1", 7) && header.e_machine == EM_AARCH64);
    CHECK(header.e_phentsize == sizeof(*phdrs) && header.e_phnum && header.e_phnum <= 32);
    CHECK(pread(fd, phdrs, header.e_phnum * sizeof(*phdrs), header.e_phoff) ==
          (ssize_t)(header.e_phnum * sizeof(*phdrs)));
    memset(image, 0, sizeof(*image));
    image->type = header.e_type;
    image->first_vaddr = ~0UL;
    unsigned long phdr_vaddr = 0;
    int found_bias = 0;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        const Elf64_Phdr *load = &phdrs[i];
        if (load->p_type == PT_INTERP && interpreter) {
            CHECK(load->p_filesz > 1 && load->p_filesz < 128);
            CHECK(pread(fd, interpreter, load->p_filesz, load->p_offset) == (ssize_t)load->p_filesz);
            CHECK(!interpreter[load->p_filesz - 1]);
        }
        if (load->p_type != PT_LOAD)
            continue;
        unsigned long first = load->p_vaddr & ~(PAGE - 1);
        if (first < image->first_vaddr)
            image->first_vaddr = first;
        if (load->p_offset <= header.e_phoff && header.e_phoff < load->p_offset + load->p_filesz)
            phdr_vaddr = load->p_vaddr + header.e_phoff - load->p_offset;
        if (!phdr_address && !found_bias && load->p_filesz) {
            for (unsigned m = 0; m < map_count; ++m) {
                if (!strcmp(maps[m].path, path) && maps[m].offset == (load->p_offset & ~(PAGE - 1))) {
                    image->bias = maps[m].start - first;
                    found_bias = 1;
                    break;
                }
            }
        }
    }
    CHECK(!close(fd) && image->first_vaddr != ~0UL);
    if (phdr_address) {
        CHECK(phdr_vaddr && phdr_address >= phdr_vaddr);
        image->bias = phdr_address - phdr_vaddr;
    } else
        CHECK(found_bias);
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        const Elf64_Phdr *load = &phdrs[i];
        if (load->p_type != PT_LOAD)
            continue;
        CHECK(image->count < EXEC_ASLR_LOADS && image->bias <= ~0UL - load->p_vaddr);
        struct exec_aslr_segment *segment = &image->loads[image->count++];
        *segment = (struct exec_aslr_segment){image->bias + load->p_vaddr, load->p_offset,
                                              load->p_filesz, load->p_memsz, load->p_align, load->p_flags};
        CHECK(!load->p_align || !(load->p_align & (load->p_align - 1)));
        CHECK(load->p_align <= 1 || !(image->bias % load->p_align));
        check_load(maps, map_count, path, segment, base, end);
    }
}

static unsigned long read_start_brk(void)
{
    int fd = open("/proc/self/stat", O_RDONLY);
    char text[2048] = {0};
    CHECK(fd >= 0 && read(fd, text, sizeof(text) - 1) > 0 && !close(fd));
    char *comm_end = strrchr(text, ')'), *save;
    CHECK(comm_end && comm_end[1] == ' ');
    char *token = strtok_r(comm_end + 2, " ", &save);
    /* Field 47 exposes mm->start_brk; the loader may already have grown brk. */
    for (unsigned field = 3; field < 47; ++field) {
        CHECK(token);
        token = strtok_r(NULL, " ", &save);
    }
    CHECK(token);
    char *end;
    unsigned long result = strtoul(token, &end, 10);
    CHECK(end > token && !*end);
    return result;
}

void exec_aslr_snapshot(const char *path, struct exec_aslr_sample *sample)
{
    memset(sample, 0, sizeof(*sample));
    sample->brk = syscall(SYS_brk, 0);
    sample->start_brk = read_start_brk();
    sample->phdr = getauxval(AT_PHDR);
    sample->entry = getauxval(AT_ENTRY);
    sample->at_base = getauxval(AT_BASE);
    sample->stack = (unsigned long)&path;
    sample->personality = personality(0xffffffffUL);
    sample->sigreturn = exec_aslr_sigreturn();
    CHECK(sample->personality != ~0UL);
    int fd = open("/probe-layout", O_RDONLY);
    char text[256] = {0}, mode[8];
    unsigned long base, end;
    CHECK(fd >= 0 && read(fd, text, sizeof(text) - 1) > 0 && !close(fd));
    CHECK(sscanf(text, "%7s %lu %lu", mode, &base, &end) == 3);
    CHECK(base <= sample->start_brk && sample->start_brk <= sample->brk && sample->brk <= end &&
          base <= sample->stack && sample->stack < end);
    struct mapping maps[128];
    unsigned map_count = read_maps(maps, 128);
    char interpreter[128] = {0};
    const char *executed = (void *)getauxval(AT_EXECFN);
    CHECK(executed);
    int direct_loader = strcmp(path, executed) != 0;
    read_image(path, direct_loader ? 0 : sample->phdr, maps, map_count, base, end, &sample->program, interpreter);
    if (*interpreter) {
        /* A directly executed loader retains the kernel's original auxv. */
        CHECK(!direct_loader || !strcmp(executed, interpreter));
        read_image(interpreter, direct_loader ? sample->phdr : 0, maps, map_count, base, end, &sample->interpreter, NULL);
        CHECK(direct_loader ? !sample->at_base : sample->at_base == sample->interpreter.bias);
    } else
        CHECK(!direct_loader && !sample->at_base);
    unsigned matches = 0;
    for (unsigned i = 0; i < map_count; ++i) {
        if (!strcmp(maps[i].path, "[nce-sigreturn]")) {
            CHECK(maps[i].end - maps[i].start == PAGE && !strcmp(maps[i].permissions, "r-xp"));
            CHECK(sample->sigreturn >= maps[i].start && sample->sigreturn < maps[i].end);
            ++matches;
        }
    }
    CHECK(matches == 1);
}

#ifdef NCE_EXEC_ASLR_IMAGE
int main(int argc, char **argv)
{
    CHECK(argc == 3 && !strcmp(argv[1], "exec-aslr-sample"));
    int fd = atoi(argv[2]);
    CHECK(fd >= 0);
    struct exec_aslr_sample sample;
    exec_aslr_snapshot(argv[0], &sample);
    const char *cursor = (void *)&sample;
    size_t left = sizeof(sample);
    while (left) {
        ssize_t bytes = write(fd, cursor, left);
        CHECK(bytes > 0);
        cursor += bytes;
        left -= bytes;
    }
    CHECK(!close(fd));
    return 0;
}
#endif
