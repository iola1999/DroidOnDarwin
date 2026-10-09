/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "nce.h"

typedef struct {
    size_t offset, size;
    NceCode code;
} NceSegment;

typedef struct { size_t address; uint32_t instruction; } NcePatch;

/* Generated from the bundled ELF files before executable addresses are
 * authorized. Linux still owns file loading, relocations and process pages. */
typedef struct {
    const char *path;
    const unsigned char *file;
    unsigned char sha256[32];
    int system_image;
    size_t size, offset, span, stub_size, entry, phoff, phsize;
    NceSegment *segments;
    size_t segment_count;
    const NcePatch *patches;
    size_t patch_count;
    uintptr_t base;
    NceCode stubs;
} NceProgram;

extern NceProgram nce_programs[];
extern const size_t nce_program_count;
NceProgram *nce_program_entry(uintptr_t entry);
unsigned long nce_program_layout(const char *path, const void *header, const void *phdrs);
int nce_program_reserved(unsigned long address, unsigned long length);
int nce_program_prepare(NceProgram *program);
int nce_program_patch_range(NceProgram *program, NceSegment *segment,
                            size_t offset, size_t size, NceState **active);
