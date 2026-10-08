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

int nce_program_patch(NceProgram *p, NceState *state, NceState **active)
{
    if (!p->stubs.rw) p->stubs = nce_memory_take(16 + 80 * p->patch_count);
    if (nce_write_entry(p->stubs, p->base + p->entry)) return -1;
    state->pc = p->base + p->entry;
    state->resume = (uintptr_t)p->stubs.rx;
    for (size_t i = 0; i < p->patch_count; ++i) {
        const NcePatch *patch = &p->patches[i];
        unsigned char *write = NULL;
        for (size_t j = 0; j < p->segment_count; ++j) {
            NceSegment *s = &p->segments[j];
            if (patch->address >= s->offset && patch->address - s->offset <= s->size - 4)
                write = s->code.rw + patch->address - s->offset;
        }
        if (!write) return -1;
        uint32_t original;
        memcpy(&original, write, 4);
        if (original != patch->instruction) return -1;
        size_t pos = 16 + i * 80;
        NceCode stub = {p->stubs.rw + pos, p->stubs.rx + pos, 80};
        if (nce_write_patch((void *)write, p->base + patch->address, stub, active, original)) return -1;
    }
    nce_sync_code(p->stubs);
    for (size_t i = 0; i < p->segment_count; ++i) nce_sync_code(p->segments[i].code);
    return 0;
}
