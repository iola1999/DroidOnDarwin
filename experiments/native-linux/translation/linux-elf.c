/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "linux-elf.h"
#include "layout.h"
#include <elf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE NCE_PAGE_SIZE
#define REQUIRE(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "runtime ELF:%d: %s\n", __LINE__, #condition); \
            abort(); \
        } \
    } while (0)
static unsigned long base;

/* Runtime ELF placement retains original code and headers. The execution address
 * range and minimum library alignment bound the number of table entries. */
typedef struct {
    char *path;
    unsigned long offset, base, span;
} ElfLayout;
static ElfLayout *layouts;
static unsigned layout_count, layout_capacity;
static unsigned long cursor;

static unsigned long elf_layout_locked(const char *path, unsigned long offset, const void *raw_header, const void *raw_phdrs)
{
    const Elf64_Ehdr *header = raw_header;
    const Elf64_Phdr *phdrs = raw_phdrs;
    if (memcmp(header->e_ident, "\177ELF\2\1\1", 7) || header->e_type != ET_DYN || header->e_machine != EM_AARCH64 ||
        header->e_phentsize != sizeof(*phdrs) || !header->e_phnum || header->e_phnum > 128 || offset % PAGE)
        return 0;
    // Bionic CFI assigns one library to each 256 KiB shadow block.
    unsigned long span = 0, alignment = 1UL << 18;
    for (unsigned i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;
        unsigned long align = phdrs[i].p_align;
        if ((align && (align & (align - 1))) || align > 2 * 1024 * 1024 || phdrs[i].p_filesz > phdrs[i].p_memsz)
            return 0;
        if (align > alignment)
            alignment = align;
        if (phdrs[i].p_vaddr > NCE_EXEC_DATA_OFFSET || phdrs[i].p_memsz > NCE_EXEC_DATA_OFFSET - phdrs[i].p_vaddr ||
            (phdrs[i].p_vaddr - phdrs[i].p_offset) % PAGE)
            return 0;
        unsigned long end = (phdrs[i].p_vaddr + phdrs[i].p_memsz + PAGE - 1) & ~(PAGE - 1);
        if (end > span)
            span = end;
    }
    for (unsigned i = 0; i < layout_count; ++i)
        if (offset == layouts[i].offset && !strcmp(path, layouts[i].path))
            return span == layouts[i].span && !(layouts[i].base % alignment) ? layouts[i].base : 0;
    unsigned long start = (base + cursor + alignment - 1) & ~(alignment - 1);
    if (!span || start - base > NCE_EXEC_DATA_OFFSET || span > NCE_EXEC_DATA_OFFSET - (start - base))
        return 0;
    if (layout_count == layout_capacity) {
        unsigned capacity = layout_capacity ? layout_capacity * 2 : 16;
        REQUIRE(capacity <= NCE_EXEC_DATA_OFFSET / (1UL << 18));
        ElfLayout *grown = realloc(layouts, capacity * sizeof(*layouts));
        REQUIRE(grown);
        layouts = grown;
        layout_capacity = capacity;
    }
    unsigned index = layout_count++;
    layouts[index].path = strdup(path);
    REQUIRE(layouts[index].path);
    layouts[index].offset = offset;
    layouts[index].base = start;
    layouts[index].span = span;
    cursor = start - base + span;
    printf("{\"phase\":\"runtime_elf\",\"path\":\"%s\",\"span_bytes\":%lu}\n", path, span);
    return layouts[index].base;
}

static unsigned long elf_offset_locked(unsigned long address)
{
    for (unsigned i = 0; i < layout_count; ++i)
        if (address >= layouts[i].base && address - layouts[i].base < layouts[i].span)
            return layouts[i].offset;
    return 0;
}

static pthread_mutex_t layout_lock = PTHREAD_MUTEX_INITIALIZER;

unsigned long translation_elf_layout(const char *path, unsigned long offset, const void *header, const void *phdrs)
{
    REQUIRE(!pthread_mutex_lock(&layout_lock));
    unsigned long result = elf_layout_locked(path, offset, header, phdrs);
    REQUIRE(!pthread_mutex_unlock(&layout_lock));
    return result;
}

unsigned long translation_elf_offset(unsigned long address)
{
    REQUIRE(!pthread_mutex_lock(&layout_lock));
    unsigned long result = elf_offset_locked(address);
    REQUIRE(!pthread_mutex_unlock(&layout_lock));
    return result;
}

void translation_elf_initialize(unsigned long user_base)
{
    REQUIRE(!layouts);
    base = user_base;
}

void translation_elf_cleanup(void)
{
    /* The caller has joined all Linux task threads. */
    for (unsigned i = 0; i < layout_count; ++i)
        free(layouts[i].path);
    free(layouts);
    layouts = NULL;
    layout_count = layout_capacity = 0;
    cursor = 0;
}
