#!/usr/bin/env python3
"""检查实际生成的 nce_prepare_user 按 CPU 发布页表、切换失效与空地址空间。"""
from pathlib import Path
import subprocess
import tempfile

from lkl_translation_patch import apply

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "nce-direct-page-table.h"
#include "nce-translation.h"
#define BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define PAGE_SHIFT DIRECT_PTE_PAGE_SHIFT
#define PMD_SHIFT DIRECT_PTE_PMD_SHIFT
#define PGDIR_SHIFT DIRECT_PTE_PGD_SHIFT
#define PTRS_PER_PGD 2048
#define PTRS_PER_PMD 2048
#define PTRS_PER_PTE 2048
#define _PAGE_PRESENT DIRECT_PTE_PRESENT
#define _PAGE_PROTNONE DIRECT_PTE_PROTNONE
#define _PAGE_RW DIRECT_PTE_WRITE
#define _PAGE_USER DIRECT_PTE_USER
#define _PAGE_ACCESSED DIRECT_PTE_YOUNG
#define _PAGE_DIRTY DIRECT_PTE_DIRTY
typedef uint64_t pgd_t, pmd_t, pte_t;
struct mm_struct { pgd_t *pgd; struct { void *flat; void *read_view; } context; } a, b, *active_mm;
struct task { struct mm_struct *mm; } task;
#define current (&task)
struct nce_translation_state shared, *translation = &shared;
struct sample_profile { int schedule; } *profile;
static int mapping_mode = NCE_MM_TRANSLATED, switch_in_lock;
static unsigned cpu_index;
#define USER_BASE 0x7000000000UL
#define TASK_SIZE (USER_BASE + (1UL << 32))
static struct nce_translation_cpu *cpu_translation(void) { return &translation->cpus[cpu_index]; }
static void nce_prepare_user(void);
static void nce_sample_runnable(int *value) { (void)value; }
static void mmap_read_lock(struct mm_struct *mm)
{
    assert(mm == current->mm);
    if (switch_in_lock) {
        switch_in_lock = 0;
        current->mm = &b;
        nce_prepare_user();
        assert(translation->cpus[cpu_index].pgd == (unsigned long)b.pgd);
        current->mm = mm;
    }
}
static void prepare_mm_locked(struct mm_struct *mm) { active_mm = mm; }
static void mmap_read_unlock(struct mm_struct *mm) { assert(mm == current->mm); }
'''

CHECKS = r'''
int main(void)
{
    pgd_t root_a[1], root_b[1];
    a.pgd = root_a;
    b.pgd = root_b;
    a.context.flat = root_a;
    b.context.flat = root_b;
    current->mm = &a;
    switch_in_lock = 1;
    nce_prepare_user();
    assert(switch_in_lock && !active_mm);
    assert(translation->cpus[0].tlb_start == USER_BASE && translation->cpus[0].tlb_end == TASK_SIZE);
    assert(translation->cpus[cpu_index].pgd == (unsigned long)a.pgd && translation->cpus[cpu_index].flat == (unsigned long)a.context.flat);
    translation->cpus[cpu_index].pgd = 0;
    nce_prepare_user();
    assert(translation->cpus[cpu_index].pgd == (unsigned long)a.pgd && translation->cpus[cpu_index].flat == (unsigned long)a.context.flat);
    current->mm = 0;
    nce_prepare_user();
    assert(translation->cpus[cpu_index].pgd == 0 && translation->cpus[cpu_index].flat == 0);
    cpu_index = 1;
    current->mm = &b;
    nce_prepare_user();
    assert(translation->cpus[1].pgd == (unsigned long)b.pgd && !translation->cpus[0].pgd);
    unsigned long mappings = translation->cpus[1].mappings;
    nce_prepare_user();
    assert(translation->cpus[1].mappings == mappings);
    cpu_index = 0;
    current->mm = &a;
    nce_prepare_user();
    assert(translation->cpus[0].pgd == (unsigned long)a.pgd);
    assert(translation->cpus[1].pgd == (unsigned long)b.pgd);
    translation->cpus[0].pgd = 0;
    switch_in_lock = 0;
    mapping_mode = 0;
    current->mm = &b;
    nce_prepare_user();
    assert(active_mm == &b && translation->cpus[cpu_index].pgd == 0);
    return 0;
}
'''


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='root-check-', dir=WORK) as directory:
        folder = Path(directory)
        (folder / 'arch/lkl/mm').mkdir(parents=True)
        headers = folder / 'arch/lkl/include/asm'
        headers.mkdir(parents=True)
        apply(folder, ROOT / 'experiments/native-linux')
        generated = (folder / 'arch/lkl/mm/nce_mmu.c').read_text()
        start = generated.index('void nce_prepare_user(void)\n{')
        end = generated.index('\n}\n', start) + 3
        source = folder / 'check.c'
        source.write_text(PRELUDE + generated[start:end] + CHECKS)
        executable = folder / 'check'
        subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', headers,
                        source, '-o', executable], check=True, timeout=30)
        subprocess.run([executable], check=True, timeout=5)
    print('translation root: per-CPU roots, switch invalidation, same mm, no mm and native mode passed')


if __name__ == '__main__':
    main()
