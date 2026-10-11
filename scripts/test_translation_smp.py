#!/usr/bin/env python3
"""检查生成内核中的代码别名生命周期、跨 CPU 失效和用户复制停止条件。"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "kernel-api.h"
#define PAGE_SHIFT 14
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & PAGE_MASK)
#define NCE_RAM_SIZE (1UL << 30)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define BUG_ON(x) assert(!(x))
typedef unsigned long pte_t;
#define __pte(x) (x)
#define pte_val(x) (x)
#define pte_present(x) ((x) & (1 | 0x10))
#define pte_exec(x) ((x) & 0x200)
struct mm_struct { int unused; };
static struct mm_struct init_mm;
static unsigned long memory_start = 0x500000000UL;
static unsigned long markers[NCE_RAM_SIZE / PAGE_SIZE];
static struct nce_translation_state state = { .cpu_count = 2, .code_guard = markers }, *translation = &state;
static unsigned int executable_ptes[NCE_RAM_SIZE / PAGE_SIZE];
static unsigned locks, stops;
static void nce_window_lock(void) { ++locks; }
static void nce_window_stop(void) { ++stops; }
static void bitmap_set(unsigned long *bits, unsigned long index, unsigned long count) {
    for (unsigned long i = index; i < index + count; ++i) bits[i / 64] |= 1UL << (i % 64);
}
'''

CHECKS = r'''
int main(void)
{
    struct mm_struct first, second;
    pte_t rx = memory_start + PAGE_SIZE + 0x201, data = memory_start + PAGE_SIZE + 1;
    nce_translation_pte_changed(&init_mm, 0, rx);
    assert(!markers[1]);
    nce_translation_pte_changed(&first, 0, rx);
    nce_translation_pte_changed(&second, 0, rx);
    assert(markers[1] == 1);
    nce_translation_pte_changed(&first, rx, rx | 0x20);
    nce_translation_pte_changed(&first, rx | 0x20, data);
    assert(markers[1] == 1);
    nce_translation_written(memory_start + PAGE_SIZE - 4, 8);
    assert(state.cpus[0].code_pages[0] == 2 && state.cpus[1].code_pages[0] == 2);
    state.cpus[0].code_pages[0] = 0;
    assert(state.cpus[1].code_pages[0] == 2);
    nce_translation_pte_changed(&second, rx, rx + PAGE_SIZE);
    assert(!markers[1] && markers[2] == 1);
    nce_translation_pte_changed(&second, rx + PAGE_SIZE, 0);
    assert(!markers[1] && !markers[2]);
    unsigned long generation = state.cpus[0].code;
    nce_translation_written(memory_start + PAGE_SIZE, PAGE_SIZE);
    assert(state.cpus[0].code == generation && !markers[1]);
    // Reuse the physical page after all executable aliases have disappeared.
    nce_translation_pte_changed(&first, data, rx);
    assert(markers[1] == 1);
    state.uaccess_code_only = 1;
    assert(!nce_translation_access_begin(memory_start + PAGE_SIZE, false));
    assert(!locks && !stops);
    assert(nce_translation_access_begin(memory_start + 2 * PAGE_SIZE, true));
    assert(locks == 1 && !stops);
    assert(nce_translation_access_begin(memory_start + PAGE_SIZE, true));
    assert(locks == 2 && stops == 1);
    state.uaccess_code_only = 0;
    assert(nce_translation_access_begin(memory_start + 2 * PAGE_SIZE, false));
    assert(locks == 3 && stops == 2);
    assert(nce_translation_access_begin(memory_start + 2 * PAGE_SIZE, true));
    assert(locks == 4 && stops == 3);
    pte_t inaccessible = (rx & ~1UL) | 0x10;
    nce_translation_pte_changed(&first, rx, inaccessible);
    assert(markers[1] == 1);
    nce_translation_pte_changed(&first, inaccessible, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(markers); ++i)
        assert(!markers[i] && !executable_ptes[i]);
}
'''


def main():
    source = WORK / 'lkl-translation-smp-kernel/source/arch/lkl/mm/nce_mmu.c'
    generated = source.read_text()
    functions = []
    for signature in ('static void changed_code(', 'void nce_translation_pte_changed(',
                      'bool nce_translation_access_begin(', 'void nce_translation_written('):
        start = generated.index(signature)
        end = generated.index('\n}\n', start) + 3
        functions.append(generated[start:end])
    with tempfile.TemporaryDirectory(prefix='translation-smp-check-', dir=WORK) as directory:
        path = Path(directory)
        (path / 'check.c').write_text(PRELUDE + '\n'.join(functions) + CHECKS)
        subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', ROOT / 'experiments/native-linux/translation',
                        path / 'check.c', '-o', path / 'check'], check=True, timeout=30)
        subprocess.run([path / 'check'], check=True, timeout=5)
    print('translation SMP: alias lifetime, physical reuse, invalidation and copy synchronization passed')


if __name__ == '__main__':
    main()
