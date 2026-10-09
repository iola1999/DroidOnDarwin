/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "programs.h"
#include "memory.h"
#include "program-images.h"
#include <string.h>

NceProgram *nce_program_entry(uintptr_t entry)
{
    for (size_t i = 0; i < nce_program_count; ++i)
        if (entry == nce_programs[i].base + nce_programs[i].entry) return &nce_programs[i];
    return NULL;
}

unsigned long nce_program_layout(const char *path, const void *header, const void *phdrs)
{
    for (size_t i = 0; i < nce_program_count; ++i) {
        NceProgram *p = &nce_programs[i];
        if (!strcmp(path, p->path) && !memcmp(header, p->file, 64) &&
            !memcmp(phdrs, p->file + p->phoff, p->phsize)) return p->base;
    }
    return 0;
}

int nce_program_reserved(unsigned long address, unsigned long length)
{
    if (!length) return 0;
    if (address + length < address) return 1;
    for (size_t i = 0; i < nce_program_count; ++i) {
        NceProgram *p = &nce_programs[i];
        uintptr_t begin = p->base + p->span;
        if (address < begin + p->stub_size && address + length > begin) return 1;
    }
    return 0;
}

int nce_program_prepare(NceProgram *p, NceState **active)
{
    if (!p->stubs.rw) return -1;
    if (nce_write_entry(p->stubs, p->base + p->entry)) return -1;
    /* Stub addresses and the active-context pointer remain fixed across mms. */
    for (size_t i = 0; i < p->patch_count; ++i) {
        const NcePatch *patch = &p->patches[i];
        if (i && patch->address <= p->patches[i - 1].address) return -1;
        size_t pos = 16 + i * 80;
        NceCode stub = {p->stubs.rw + pos, p->stubs.rx + pos, 80};
        uint32_t branch;
        if (nce_write_patch(&branch, p->base + patch->address, stub, active, patch->instruction)) return -1;
    }
    nce_sync_code(p->stubs);
    return 0;
}

int nce_program_patch_range(NceProgram *p, NceSegment *segment,
                            size_t offset, size_t size)
{
    size_t begin = segment->offset + offset, end = begin + size;
    size_t lo = 0, hi = p->patch_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->patches[mid].address < begin) lo = mid + 1;
        else hi = mid;
    }
    for (size_t i = lo; i < p->patch_count && p->patches[i].address < end; ++i) {
        const NcePatch *patch = &p->patches[i];
        uint32_t *write = (void *)(segment->code.rw + patch->address - segment->offset);
        if (*write != patch->instruction) return -1;
        if (nce_write_branch(write, p->base + patch->address,
                             (uintptr_t)p->stubs.rx + 16 + i * 80)) return -1;
    }
    return 0;
}
