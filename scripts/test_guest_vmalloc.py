#!/usr/bin/env python3
"""用实际 LKL VMALLOC 对象检查共享别名与部分失败释放。"""
import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('summary', type=Path, help='Successful 4 KiB core probe summary')
    args = parser.parse_args()
    baseline = json.loads(args.summary.read_text())
    if baseline['configuration']['guest_page_bytes'] != 4096 or not all(run['validated'] for run in baseline['runs']):
        parser.error('Requires a successful 4 KiB baseline')
    for name, value in baseline['sha256'].items():
        if digest(ROOT / name) != value:
            raise RuntimeError('Baseline input changed: ' + name)
    kernel = WORK / 'lkl-translation-kernel-4k-vmalloc'
    build = json.loads((kernel / 'build.json').read_text())
    for name, value in build['sha256'].items():
        if digest(ROOT / name) != value:
            raise RuntimeError('Kernel input changed: ' + name)
    out = WORK / 'guest-4k-vmalloc-tests'
    out.mkdir(exist_ok=True)
    llvm = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    commands, results = [], []

    def run(command, cwd=ROOT):
        command = list(map(str, command))
        commands.append({'argv': command, 'cwd': str(cwd)})
        subprocess.run(command, cwd=cwd, check=True, timeout=120)

    source = (kernel / 'source/mm/vmalloc.c').read_text()
    changes = [
        ('\t\tif (nid == NUMA_NO_NODE)\n\t\t\tpage = alloc_pages_noprof(gfp, order);',
         '\t\tif (current == nce_test_task && nce_test_alloc_left >= 0 && !nce_test_alloc_left--)\n\t\t\tbreak;\n\t\tif (nid == NUMA_NO_NODE)\n\t\t\tpage = alloc_pages_noprof(gfp, order);'),
        ('\t\tif (unlikely(!page))\n\t\t\tbreak;',
         '\t\tif (unlikely(!page))\n\t\t\tbreak;\n\t\tif (current == nce_test_task)\n\t\t\tnce_test_allocated += 1U << order;'),
        ('\t\tset_pte_at(&init_mm, addr, pte, mk_pte(page, prot));',
         '\t\tif (current == nce_test_task && nce_test_map_left >= 0 && !nce_test_map_left--)\n\t\t\treturn -ENOMEM;\n\t\tset_pte_at(&init_mm, addr, pte, mk_pte(page, prot));'),
        ('\t\t__free_page(page);',
         '\t\tif (current == nce_test_task)\n\t\t\tnce_test_freed++;\n\t\t__free_page(page);'),
        ('\tarray_size = (unsigned long)nr_small_pages * sizeof(struct page *);',
         '\tif (current == nce_test_task)\n\t\tnce_test_area = addr;\n\tarray_size = (unsigned long)nr_small_pages * sizeof(struct page *);'),
    ]
    for before, after in changes:
        if source.count(before) != 1:
            raise RuntimeError('VMALLOC test insertion mismatch: ' + before)
        source = source.replace(before, after)
    source = ('struct task_struct;\nstatic struct task_struct *nce_test_task;\n'
              'static int nce_test_alloc_left = -1, nce_test_map_left = -1;\n'
              'static unsigned nce_test_allocated, nce_test_freed;\n'
              'static unsigned long nce_test_area;\n' + source + '\n' +
              (ROOT / 'tests/native-linux/kernel-vmalloc.c').read_text())
    (out / 'vmalloc-test.c').write_text(source)
    (out / 'abort.c').write_text('#include <stdlib.h>\n_Noreturn void nce_test_abort(void) { _Exit(86); }\n')
    entries = 'lkl_start_kernel lkl_sys_halt lkl_syscall lkl_trigger_irq lkl_get_free_irq lkl_put_irq lkl_is_running lkl_bug lkl_printf lkl_init lkl_cleanup'.split()
    original_objects = subprocess.check_output([llvm / 'llvm-ar', 't', kernel / 'build/vmlinux.a'], text=True).splitlines()
    original_object = str(kernel / 'build/mm/vmalloc.o')
    if original_objects.count(original_object) != 1:
        raise RuntimeError('Expected one VMALLOC object')
    compile_source = shlex.split((kernel / 'build/mm/.vmalloc.o.cmd').read_text().splitlines()[0].split(' := ', 1)[1])
    compile_rejections = []
    for option in ('CONFIG_HAVE_ARCH_HUGE_VMALLOC', 'CONFIG_HAVE_ARCH_HUGE_VMAP'):
        command = [str(out / 'vmalloc-test.c') if x == str(kernel / 'source/mm/vmalloc.c') else
                   str(out / 'unsupported.o') if x == 'mm/vmalloc.o' else
                   f'-Wp,-MMD,{out}/unsupported.d' if x.startswith('-Wp,-MMD,') else x for x in compile_source]
        command.append('-D' + option + '=1')
        commands.append({'argv': command, 'cwd': str(kernel / 'build')})
        result = subprocess.run(command, cwd=kernel / 'build', text=True, capture_output=True, timeout=60)
        valid = result.returncode != 0 and '4 KiB LKL VMALLOC requires ordinary PTE mappings' in result.stderr
        compile_rejections.append({'option': option, 'returncode': result.returncode, 'validated': valid, 'stderr': result.stderr})
        if not valid:
            raise RuntimeError('Unsupported huge-vmap configuration was not rejected')
    host_commands = {entry: next(c for c in baseline['commands'] if str(args.summary.resolve().parent / entry) in c) for entry in ('cli', 'gcd')}
    environment = {**os.environ, 'NCE_TRANSLATION_FILES': str(args.summary.resolve().parent),
                   'NCE_TRANSLATION_WORKLOAD': 'core', 'NCE_TRANSLATION_GUEST_LAYOUT': 'low',
                   'NCE_TRANSLATION_MEMORY_MS': '100'}
    variants = {'normal': [], 'nofail': ['-DNCE_VMALLOC_NOFAIL_TEST'],
                'mixed-permissions': ['-DNCE_VMALLOC_PERMISSION_TEST']}
    for variant, flags in variants.items():
        directory = out / variant
        directory.mkdir(exist_ok=True)
        command = [str(out / 'vmalloc-test.c') if x == str(kernel / 'source/mm/vmalloc.c') else
                   str(directory / 'vmalloc.o') if x == 'mm/vmalloc.o' else
                   f'-Wp,-MMD,{directory}/vmalloc.d' if x.startswith('-Wp,-MMD,') else x for x in compile_source]
        run([*command, *flags], kernel / 'build')
        objects = [str(directory / 'vmalloc.o') if x == original_object else x for x in original_objects]
        run([llvm / 'llvm-ar', 'cDPrST', directory / 'vmlinux.a', *objects])
        run(['/opt/homebrew/bin/ld.lld', '-z', 'noexecstack', '-r', '--build-id=sha1',
             '--script=' + str(kernel / 'build/arch/lkl/kernel/vmlinux.lds'), '-o', directory / 'vmlinux',
             '--whole-archive', directory / 'vmlinux.a', kernel / 'build/init/version-timestamp.o',
             '--no-whole-archive', '--start-group', kernel / 'build/lib/lib.a', '--end-group'])
        run([llvm / 'llvm-objcopy', '-R', '.eh_frame', '-R', '.syscall_defs',
             *['-G' + name for name in entries], directory / 'vmlinux', directory / 'lkl.o'])
        for command in build['commands'][3:]:
            run([str(directory / 'lkl.o') if x == str(kernel / 'build/lkl.o') else
                 x.replace(str(kernel), str(directory)) if x in (str(kernel / 'lkl-exports.o'), str(kernel / 'lkl.so')) else x for x in command])
        import probe_lkl
        probe_lkl.OUT, probe_lkl.PROCESS, probe_lkl.SMP = directory, True, False
        probe_lkl.package_elf(directory / 'lkl.so')
        symbols = subprocess.check_output([llvm / 'llvm-nm', directory / 'lkl.so'], text=True)
        elf = (directory / 'lkl.so').read_bytes()
        image = (directory / 'lkl-image.bin').read_bytes()
        phoff = struct.unpack_from('<Q', elf, 32)[0]
        phsize, phcount = struct.unpack_from('<HH', elf, 54)
        header = ''
        for constant, name in {'TRANSLATION_COPY_USER': 'nce_copy_user', 'TRANSLATION_LOOKUP': 'nce_translate_user'}.items():
            match = re.search(r'^([0-9a-f]+) [tT] ' + name + '$', symbols, re.M)
            if not match:
                raise RuntimeError('Test kernel symbol missing: ' + name)
            address = int(match[1], 16)
            segments = [struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phsize) for i in range(phcount)]
            if not any(kind == 1 and flags & 1 and start <= address and address + 64 <= start + size and
                       elf[offset + address - start:offset + address - start + 64] == image[address:address + 64]
                       for kind, flags, offset, start, _, size, _, _ in segments):
                raise RuntimeError('Test kernel symbol/image mismatch: ' + name)
            header += f'#define {constant} 0x{address:x}UL\n'
        (directory / 'translation-symbols.h').write_text(header)
        for entry, command in host_commands.items():
            command = [str(directory) if x == str(kernel) else str(directory / entry) if x == str(args.summary.resolve().parent / entry) else x for x in command]
            run([*command, '-I', kernel, '-Dabort=nce_test_abort', out / 'abort.c'])
            argv = [str(directory / entry), str(directory / 'lkl-image.bin')]
            with (directory / f'{entry}.jsonl').open('w') as stdout, (directory / f'{entry}.log').open('w') as stderr:
                process = subprocess.run(argv, env=environment, stdout=stdout, stderr=stderr, timeout=120)
            text = (directory / f'{entry}.log').read_text()
            rows = [json.loads(line.removeprefix('NCE_VMALLOC_TEST ')) for line in text.splitlines() if line.startswith('NCE_VMALLOC_TEST {')]
            if variant == 'normal':
                expected = [
                    {'test': 'vmalloc_rounding_guard', 'passed': True},
                    {'test': 'vmalloc_shared_alias_neighbor', 'passed': True},
                    {'test': 'vmalloc_invalid_groups', 'passed': True},
                    *[{'test': 'vmalloc_allocation_failure', 'after_groups': n, 'allocated': n * 4, 'freed': n * 4, 'passed': True} for n in (0, 1)],
                    *[{'test': 'vmalloc_partial_map_failure', 'after_pages': n, 'allocated': 8, 'freed': 8, 'passed': True} for n in (1, 3, 5)],
                    {'test': 'kernel_vmalloc', 'passed': True}]
                valid = process.returncode == 0 and rows == expected
            else:
                marker = 'nofail_requested' if variant == 'nofail' else 'mixed_permissions_requested'
                expected = [] if variant == 'nofail' else [{'test': 'vmalloc_rounding_guard', 'passed': True}]
                valid = (process.returncode == 86 and rows == expected and
                         'NCE_VMALLOC_TEST ' + marker in text and
                         'translated_linux_boot' not in (directory / f'{entry}.jsonl').read_text() and
                         (variant != 'nofail' or '4 KiB VMALLOC rejects NOFAIL: bytes=1 ' in text))
            results.append({'variant': variant, 'entry': entry, 'command': argv, 'returncode': process.returncode,
                            'validated': valid, 'tests': rows, 'stdout': (directory / f'{entry}.jsonl').read_text(), 'stderr': text})
            print(variant, entry, process.returncode, valid, flush=True)
    for name, value in build['sha256'].items():
        if digest(ROOT / name) != value:
            raise RuntimeError('Baseline kernel changed during test: ' + name)
    inputs = [Path(__file__).resolve(), ROOT / 'tests/native-linux/kernel-vmalloc.c', args.summary.resolve(),
              kernel / 'build.json', out / 'vmalloc-test.c', out / 'abort.c']
    inputs += [out / variant / name for variant in variants for name in ('vmalloc.o', 'lkl.so', 'lkl-image.bin', 'lkl-image.h', 'translation-symbols.h', 'cli', 'gcd')]
    report = {'scope': 'LKL VMALLOC tests; failure counters exist only in the replacement test object',
              'commands': commands, 'environment': {k: v for k, v in environment.items() if k.startswith('NCE_')},
              'baseline_kernel_inputs_unchanged': True, 'compile_rejections': compile_rejections, 'runs': results,
              'sha256': {str(p.relative_to(ROOT)): digest(p) for p in inputs}}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    if not all(row['validated'] for row in results):
        raise SystemExit('VMALLOC regression failed')


if __name__ == '__main__':
    main()
