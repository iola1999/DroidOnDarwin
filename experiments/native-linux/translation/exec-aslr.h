/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_EXEC_ASLR_TEST_H
#define NCE_EXEC_ASLR_TEST_H

#define EXEC_ASLR_LOADS 16

struct exec_aslr_segment {
    unsigned long address, offset, filesz, memsz, align;
    unsigned flags;
};

struct exec_aslr_image {
    unsigned long bias, first_vaddr;
    unsigned type, count;
    struct exec_aslr_segment loads[EXEC_ASLR_LOADS];
};

struct exec_aslr_sample {
    unsigned long phdr, entry, at_base, start_brk, brk, stack, sigreturn, personality;
    struct exec_aslr_image program, interpreter;
};

unsigned long exec_aslr_sigreturn(void);
void exec_aslr_snapshot(const char *path, struct exec_aslr_sample *sample);
void exec_aslr_check_mapped(unsigned long start, unsigned long end);

#endif
