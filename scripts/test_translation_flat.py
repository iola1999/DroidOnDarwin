#!/usr/bin/env python3
"""检查实际内核地址表函数的权限、重映射、刷新与 mm 生命周期。"""
from pathlib import Path
import subprocess
import tempfile

from lkl_translation_patch import apply

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'

PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nce-direct-page-table.h"
#include "nce-translation.h"
#define PAGE_SHIFT 14
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & PAGE_MASK)
#define USER_BASE 0x7000000000UL
#define NCE_USER_SIZE (1UL << 32)
#define TASK_SIZE (USER_BASE + NCE_USER_SIZE)
#define NCE_RAM_SIZE (1UL << 30)
#define max(a,b) ((a) > (b) ? (a) : (b))
#define min(a,b) ((a) < (b) ? (a) : (b))
typedef unsigned long pte_t;
#define pte_val(x) (x)
#define pte_present(x) ((x) & DIRECT_PTE_PRESENT)
#define pte_read(x) (((x) & DIRECT_PTE_USER) && !((x) & DIRECT_PTE_PROTNONE))
#define pte_write(x) (((x) & DIRECT_PTE_WRITE) && !((x) & DIRECT_PTE_PROTNONE))
#define pte_young(x) ((x) & DIRECT_PTE_YOUNG)
#define pte_dirty(x) ((x) & DIRECT_PTE_DIRTY)
struct task_struct { int unused; };
struct mm_struct {
    struct { unsigned long *accessed; struct nce_direct_flat_page *flat; } context;
    pte_t ptes[4];
};
static struct nce_translation_state state, *translation = &state;
static unsigned long memory_start = TASK_SIZE, mapping_mode = NCE_MM_TRANSLATED;
static struct mm_struct *active_mm;
static int fail_alloc;
static void *allocate(unsigned long size) { return fail_alloc ? NULL : calloc(1, size); }
static void release(void *p, unsigned long size) { assert(size == DIRECT_FLAT_BYTES); free(p); }
static struct {
    void *(*page_alloc)(unsigned long);
    void (*page_free)(void *, unsigned long);
    void *(*mem_alloc)(size_t);
    void (*mem_free)(void *);
} ops = {allocate, release, malloc, free}, *lkl_ops = &ops;
static pte_t lookup(struct mm_struct *mm, unsigned long address)
{
    unsigned long index = (address - USER_BASE) >> PAGE_SHIFT;
    return index < 4 ? mm->ptes[index] : 0;
}
'''

CHECKS = r'''
int main(void)
{
    struct mm_struct a = {0}, b = {0}, failed = {0};
    state.flat_enabled = 1;
    state.ram = 0x100000000UL;
    assert(!nce_init_mm(NULL, &a) && !nce_init_mm(NULL, &b));
    assert(a.context.flat != b.context.flat);
    assert(state.flat_live_bytes == 2 * DIRECT_FLAT_BYTES);
    assert(state.flat_peak_bytes == state.flat_live_bytes);
    fail_alloc = 1;
    assert(nce_init_mm(NULL, &failed) == -ENOMEM && !failed.context.flat);
    assert(state.flat_live_bytes == 2 * DIRECT_FLAT_BYTES);
    pte_t valid = memory_start + PAGE_SIZE + DIRECT_PTE_WRITE_REQUIRED;
    flat_update(&a, USER_BASE, valid);
    assert(a.context.flat[0].read == state.ram + PAGE_SIZE);
    assert(a.context.flat[0].write == state.ram + PAGE_SIZE && !b.context.flat[0].read);
    unsigned long updates = state.flat_updates;
    flat_update(&a, USER_BASE, valid);
    flat_update(&a, USER_BASE - PAGE_SIZE, valid);
    flat_update(&a, TASK_SIZE, valid);
    assert(state.flat_updates == updates);
    unsigned long denied[] = {DIRECT_PTE_PRESENT, DIRECT_PTE_USER, DIRECT_PTE_YOUNG};
    for (unsigned i = 0; i < 3; ++i) {
        flat_update(&a, USER_BASE, valid & ~denied[i]);
        assert(!a.context.flat[0].read && !a.context.flat[0].write);
    }
    flat_update(&a, USER_BASE, valid | DIRECT_PTE_PROTNONE);
    assert(!a.context.flat[0].read && !a.context.flat[0].write);
    flat_update(&a, USER_BASE, valid & ~DIRECT_PTE_DIRTY);
    assert(a.context.flat[0].read && !a.context.flat[0].write);
    flat_update(&a, USER_BASE, valid & ~DIRECT_PTE_WRITE);
    assert(a.context.flat[0].read && !a.context.flat[0].write);
    flat_update(&a, USER_BASE, memory_start - PAGE_SIZE + DIRECT_PTE_WRITE_REQUIRED);
    assert(!a.context.flat[0].read && !a.context.flat[0].write);
    flat_update(&a, USER_BASE, memory_start + NCE_RAM_SIZE + DIRECT_PTE_WRITE_REQUIRED);
    assert(!a.context.flat[0].read && !a.context.flat[0].write);
    flat_update(&a, USER_BASE, valid);
    a.ptes[0] = valid + PAGE_SIZE;
    a.ptes[1] = valid & ~DIRECT_PTE_DIRTY;
    flat_refresh(&a, USER_BASE + 7, USER_BASE + PAGE_SIZE + 1);
    assert(a.context.flat[0].write == state.ram + 2 * PAGE_SIZE);
    assert(a.context.flat[1].read == state.ram + PAGE_SIZE && !a.context.flat[1].write);
    a.ptes[0] = a.ptes[1] = 0;
    flat_refresh(&a, USER_BASE, USER_BASE + 2 * PAGE_SIZE);
    assert(!a.context.flat[0].read && !a.context.flat[1].read);
    assert(state.flat_refresh_pages == 4);
    active_mm = &a;
    nce_destroy_mm(&a);
    assert(!active_mm && !a.context.flat && state.flat_live_bytes == DIRECT_FLAT_BYTES);
    nce_destroy_mm(&a);
    assert(state.flat_live_bytes == DIRECT_FLAT_BYTES);
    nce_destroy_mm(&b);
    assert(!state.flat_live_bytes);
    state.flat_enabled = 0;
    fail_alloc = 0;
    assert(!nce_init_mm(NULL, &a) && !a.context.flat);
    nce_destroy_mm(&a);
    assert(!state.flat_live_bytes);
}
'''


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flat-check-', dir=WORK) as directory:
        folder = Path(directory)
        (folder / 'arch/lkl/mm').mkdir(parents=True)
        headers = folder / 'arch/lkl/include/asm'
        headers.mkdir(parents=True)
        apply(folder, ROOT / 'experiments/native-linux')
        generated = (folder / 'arch/lkl/mm/nce_mmu.c').read_text()
        functions = []
        for signature in ('int nce_init_mm(', 'void nce_destroy_mm(', 'static void flat_update(', 'static void flat_refresh('):
            start = generated.index(signature)
            end = generated.index('\n}\n', start) + 3
            functions.append(generated[start:end])
        source = folder / 'check.c'
        source.write_text(PRELUDE + '\n'.join(functions) + CHECKS)
        executable = folder / 'check'
        subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Werror', '-I', headers,
                        source, '-o', executable], check=True, timeout=30)
        subprocess.run([executable], check=True, timeout=5)
    print('translation flat: permissions, remap, range refresh, allocation failure and mm lifecycle passed')


if __name__ == '__main__':
    main()
