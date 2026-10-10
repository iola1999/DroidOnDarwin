#!/usr/bin/env python3
"""用软件 TLB 执行真实 LKL 用户进程，记录 CLI/GCD 功能与内存结果。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
WORK = ROOT / 'work/native-linux-nce'
KERNEL = WORK / 'processes'
DEFAULT_OUT = WORK / 'lkl-translation'


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, timeout=120, **kwargs)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate(rows, memory_interval, mapping="window", workload="core"):
    expected = {'linux_dynamic_code', 'linux_signal_suite', 'linux_signal_register_restore',
                'linux_signal_eintr_restart', 'linux_signal_timer_preemption', 'linux_mm_exchange_integrity',
                'translated_pthread_tls_mutex', 'translated_signals_protection_restore',
                'translated_runtime_elf_exec', 'translated_runtime_libraries', 'translated_context_cleanup',
                'linux_ram_capacity_cow', 'linux_brk_growth_cow', 'linux_fault_contiguous_data_boundaries',
                'linux_exec_symlink', 'linux_aarch64_file_open_flags', 'linux_stack_growth_and_limit',
                'linux_android_elf_reservation_boundaries', 'linux_sparse_reservation_cow',
                'linux_mmap_byte_offset', 'linux_fault_register_state', 'linux_user_memory_boundaries',
                'linux_dynamic_link_dlopen_constructors', 'linux_dynamic_tls_threads', 'linux_dynamic_fork_signal'}
    if workload == 'android':
        expected = {'android_system_image_readonly', 'android_bionic_threads_allocator_tls',
                    'android_bionic_many_threads', 'android_scudo_class_growth', 'linux_proc_concurrent_exit',
                    'android_cfi_library_reload', 'android_apk_native_libraries', 'android_cfi_bounded_shadow',
                    'android_art_library_loaded', 'android_art_java_workload', 'android_art_jit_arithmetic',
                    'android_art_jit_objects', 'android_art_jit_calls', 'android_art_jit_allocations',
                    'android_art_jit_automatic', 'android_art_jit_runtime_features', 'android_art_jit_patterns',
                    'android_art_jit_strings_classes', 'android_art_jit_types', 'android_art_jit_intrinsics',
                    'android_art_proxy_references', 'android_art_vm_destroyed'}
    if not rows or rows[-1].get('result') != 'passed' or any(row.get('passed') is False for row in rows):
        return False
    if not expected <= {row.get('test') for row in rows if row.get('passed')}:
        return False
    cleanup = [row for row in rows if row.get('test') == 'translated_context_cleanup']
    if len(cleanup) != 1 or (mapping == 'pte' and (cleanup[0]['maps'] or cleanup[0]['revocations'])):
        return False
    cases = [(row['bytes'], row['shared_percent'], row['touch_all'], row['threads'])
             for row in rows if row.get('bench') == 'linux_mm_exchange']
    expected_cases = {(size * 1024 * 1024, shared, touch, 0)
                      for size in (2, 4, 8, 16) for shared in (0, 50, 100) for touch in (0, 1)}
    expected_cases.update((size * 1024 * 1024, 0, 1, 1) for size in (2, 4, 8, 16))
    if workload == 'core' and (len(cases) != 28 or set(cases) != expected_cases):
        return False
    phases = ('kernel_setup', 'guest_setup', 'guest_execution', 'cleanup')
    if workload == 'android':
        phases = ('kernel_setup', 'guest_setup', 'bionic_execution', 'cfi_execution', 'art_execution', 'cleanup')
    summaries = [row for row in rows if 'memory_statistics' in row]
    if not memory_interval:
        return not summaries and not any('memory_sample' in row for row in rows)
    if [row['memory_statistics'] for row in summaries] != list(phases):
        return False
    for summary in summaries:
        samples = [row for row in rows if row.get('memory_sample') == summary['memory_statistics']]
        if summary['errors'] or len(samples) != summary['samples'] or len(samples) < 2:
            return False
        if [row['monotonic_ns'] for row in samples] != sorted(row['monotonic_ns'] for row in samples):
            return False
        for metric in ('footprint', 'resident'):
            values = [row[metric + '_bytes'] for row in samples]
            if (summary[metric + '_latest_bytes'] != values[-1] or
                summary[metric + '_sample_peak_bytes'] != max(values) or
                abs(summary[metric + '_mean_bytes'] - sum(values) / len(values)) > 0.001):
                return False
        if summary['resident_lifetime_peak_bytes'] != samples[-1]['resident_lifetime_peak_bytes']:
            return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--mapping', choices=('window', 'pte'), default='window')
    parser.add_argument('--workload', choices=('core', 'android'), default='core')
    parser.add_argument('--art-nterp', type=int, choices=(0, 1), default=1)
    parser.add_argument('--art-expanded-jit', type=int, choices=(0, 1), default=0)
    parser.add_argument('--art-fast-baseline', type=int, choices=(0, 1), default=0)
    parser.add_argument('--cache-invalidation', choices=('full', 'pages'), default='pages')
    parser.add_argument('--linux-fastpath', type=int, choices=(0, 1), default=0)
    parser.add_argument('--cache-mib', type=int, choices=(16, 64, 128), default=16)
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--batch-instructions', type=int, default=50000)
    parser.add_argument('--preemption', choices=('instructions', 'timer'), default='instructions')
    parser.add_argument('--output', type=Path, default=DEFAULT_OUT)
    parser.add_argument('--memory-interval-ms', type=int, default=100)
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(WORK):
        parser.error('实验产物必须放在本项目 work/native-linux-nce 下')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or args.repeat < 1:
        parser.error('需要 Apple Silicon Mac，repeat 必须为正数')
    if not 1000 <= args.batch_instructions <= 1000000 or not (args.memory_interval_ms == 0 or 10 <= args.memory_interval_ms <= 10000):
        parser.error('指令批次范围 1000…1000000；内存采样间隔为 0 或 10…10000 ms')
    from build_translation_engine import build as build_engine
    dependency = build_engine()
    library = Path(dependency['path'])
    out.mkdir(parents=True, exist_ok=True)
    llvm = Path('/opt/homebrew/opt/llvm/bin')
    kernel = KERNEL
    if args.mapping == 'pte':
        from build_lkl_translation_kernel import build
        kernel = build()
    else:
        native_manifest = json.loads((kernel / 'lkl-summary.json').read_text())
        if sha(kernel / 'lkl-image.bin') != native_manifest['kernel_sha256'] or any(
            sha(kernel / name) != native_manifest['generated_sha256'][name] for name in ('lkl-image.h', 'lkl-host-ops.h')):
            parser.error('内核镜像或头文件与本机验证记录不一致，请先重新构建并验证单核内核')
    symbols = subprocess.check_output([llvm / 'llvm-nm', kernel / 'lkl.so'], text=True)
    elf = (kernel / 'lkl.so').read_bytes()
    image = (kernel / 'lkl-image.bin').read_bytes()
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', elf, 54)
    exports = {'TRANSLATION_COPY_USER': 'nce_copy_user'}
    if args.mapping == 'pte':
        exports['TRANSLATION_LOOKUP'] = 'nce_translate_user'
    header = '' if args.mapping == 'pte' else '#define TRANSLATION_LOOKUP 0UL\n'
    for constant, name in exports.items():
        match = re.search(r'^([0-9a-f]+) [tT] ' + name + '$', symbols, re.M)
        if not match:
            parser.error('内核缺少 ' + name)
        symbol = int(match[1], 16)
        matched = False
        for index in range(phcount):
            kind, flags, offset, address, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + index * phsize)
            if kind == 1 and flags & 1 and address <= symbol and symbol + 64 <= address + size:
                matched = elf[offset + symbol - address:offset + symbol - address + 64] == image[symbol:symbol + 64]
        if not matched:
            parser.error(name + ' 的 ELF 地址与内核镜像不一致')
        header += f'#define {constant} 0x{symbol:x}UL\n'
    (out / 'translation-symbols.h').write_text(header)
    inputs = [EXP / 'translation/linux-execution.c', EXP / 'translation/linux-runner.c', EXP / 'translation/linux-memory.c', EXP / 'lkl-host.c']
    commands = []
    for mode in ('cli', 'gcd'):
        command = ['xcrun', 'clang', '-O2', '-g', '-DNCE_PROCESS', '-Wall', '-Wextra', '-Werror',
                   '-I', EXP, '-I', kernel, '-I', out, '-I', KERNEL / 'host-include', '-I', library / 'include',
                   *inputs, library / 'lib/libunicorn.a', '-o', out / mode]
        if mode == 'gcd':
            command += ['-DTRANSLATION_DISPATCH', EXP / 'dispatch-test.c']
        run(command)
        commands.append(list(map(str, command)))
    # Guest files are built after both hosts and are never linked into them.
    hosts_before = {mode: sha(out / mode) for mode in ('cli', 'gcd')}
    libc = WORK / 'musl-build/install'
    resource = Path(subprocess.check_output([llvm / 'clang', '-print-resource-dir'], text=True).strip()) / 'include'
    command = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
               '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
               '-c', EXP / 'translation/linux-guest.c', '-o', out / 'guest.o']
    run(command)
    commands.append(list(map(str, command)))
    guest_objects = []
    guest_sources = ('signal-tests.c', 'signal-registers.S', 'code-tests.c', 'mm-bench.c', 'mm-tests.c', 'file-tests.c')
    for name in guest_sources:
        obj = out / (Path(name).stem + '.o')
        compile_command = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
                           '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-Dmain=unused_mm_main', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
                           '-c', EXP / 'userspace' / name, '-o', obj]
        run(compile_command)
        commands.append(list(map(str, compile_command)))
        guest_objects.append(obj)
    command = [shutil.which('ld.lld'), '-pie', '--gc-sections', '--no-dynamic-linker', '-z', 'max-page-size=16384', '-z', 'common-page-size=16384',
               libc / 'lib/rcrt1.o', libc / 'lib/crti.o', out / 'guest.o', *guest_objects, libc / 'lib/libc.a',
               WORK / 'compiler-rt-build/libbuiltins.a', libc / 'lib/crtn.o', '-o', out / 'guest.elf']
    run(command)
    commands.append(list(map(str, command)))
    run([llvm / 'llvm-strip', '--strip-all', out / 'guest.elf', '-o', out / 'late.elf'])
    runtime_files = ('sigreturn.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so', 'dash.elf', 'bionic-test.elf')
    for name in runtime_files:
        shutil.copyfile(KERNEL / name, out / name)
    android_files = []
    if args.workload == 'android':
        for name, source in [('NceWorkload.dex', KERNEL / 'java-classes/classes.dex'),
                             ('ld.config.txt', EXP / 'art/ld.config.txt')]:
            shutil.copyfile(source, out / name)
            android_files.append(out / name)
        for name in ('art-nterp', 'art-expanded-jit', 'art-fast-baseline'):
            (out / name).write_text(str(getattr(args, name.replace('-', '_'))))
            android_files.append(out / name)
        android_files.append(KERNEL / 'system.img')
    if hosts_before != {mode: sha(out / mode) for mode in hosts_before}:
        raise RuntimeError('客体构建后宿主文件发生变化')
    files = [*inputs, EXP / 'translation/linux-execution.h', EXP / 'translation/linux-memory.h', EXP / 'translation/linux-guest.c', Path(__file__),
             EXP / 'dispatch-test.c', EXP / 'layout.h', EXP / 'guest-registers.h',
             *[EXP / 'userspace' / name for name in guest_sources], *android_files,
             kernel / 'lkl-image.bin', kernel / 'lkl-image.h', kernel / 'lkl.so', kernel / 'lkl-host-ops.h',
             EXP / 'translation/kernel-api.h',
             ROOT / 'scripts/build_translation_engine.py', WORK / 'unicorn-tb-stop/build.json',
             library / 'lib/libunicorn.a', *[out / name for name in ('cli', 'gcd', 'guest.elf', 'late.elf', *runtime_files)]]
    if args.mapping == 'pte':
        files += [ROOT / 'scripts/build_lkl_translation_kernel.py', ROOT / 'scripts/lkl_translation_patch.py',
                  EXP / 'lkl-nce-mmu.c', kernel / 'build.json']
    input_hashes = {str(path.relative_to(ROOT)): sha(path) for path in files}
    if args.build_only:
        print(out)
        return
    environment = os.environ.copy()
    environment['NCE_TRANSLATION_FILES'] = str(out)
    environment['NCE_TRANSLATION_BATCH'] = str(args.batch_instructions)
    environment['NCE_TRANSLATION_PREEMPTION'] = args.preemption
    environment['NCE_TRANSLATION_MEMORY_MS'] = str(args.memory_interval_ms)
    environment['NCE_TRANSLATION_WORKLOAD'] = args.workload
    environment['NCE_TRANSLATION_SYSTEM'] = str(KERNEL / 'system.img')
    environment['NCE_TRANSLATION_CACHE'] = args.cache_invalidation
    environment['NCE_TRANSLATION_LINUX_FASTPATH'] = str(args.linux_fastpath)
    environment['NCE_TRANSLATION_CACHE_MIB'] = str(args.cache_mib)
    results = []
    for iteration in range(args.repeat):
        for mode in ('cli', 'gcd'):
            stem = f'{mode}-{iteration}'
            with (out / f'{stem}.jsonl').open('w') as output, (out / f'{stem}.log').open('w') as error:
                try:
                    process = subprocess.run([out / mode, kernel / 'lkl-image.bin'], env=environment,
                                             stdout=output, stderr=error, timeout=120)
                    returncode = process.returncode
                except subprocess.TimeoutExpired:
                    returncode = 'timeout'
            rows = [json.loads(line) for line in (out / f'{stem}.jsonl').read_text().splitlines() if line.startswith('{')]
            passed = returncode == 0 and validate(rows, args.memory_interval_ms, args.mapping, args.workload)
            results.append({'entry': mode, 'iteration': iteration, 'returncode': returncode, 'results': rows,
                            'validated': passed,
                            'stdout_sha256': sha(out / f'{stem}.jsonl'), 'stderr_sha256': sha(out / f'{stem}.log')})
            if not passed:
                break
        if not results[-1]['validated']:
            break
    if input_hashes != {str(path.relative_to(ROOT)): sha(path) for path in files}:
        raise SystemExit('本轮验证期间源码或二进制发生变化，请重跑')
    report = {'scope': 'Native single-CPU LKL with translated ARM64 userspace; no Android/iPhone integration',
              'platform': platform.platform(), 'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'dependency': dependency, 'commands': commands, 'host_unchanged_after_guest_build': True,
              'configuration': {'mapping': args.mapping, 'sparse_scan': True, 'fault_prefetch': False, 'guest_page_bytes': 16384,
                                'workload': args.workload, 'art_nterp': args.art_nterp,
                                'art_expanded_jit': args.art_expanded_jit, 'art_fast_baseline': args.art_fast_baseline,
                                'cache_invalidation': args.cache_invalidation,
                                'linux_fastpath': args.linux_fastpath,
                                'guest_ram_bytes': 1073741824, 'translation_cache_bytes': args.cache_mib * 1024 * 1024,
                                'instruction_batch': args.batch_instructions, 'memory_interval_ms': args.memory_interval_ms,
                                'preemption': args.preemption, 'timer_interval_ns': 2000000},
              'measurement': 'Guest clock times 128 pipe round trips per memory case; excludes initial COW materialization. Selected preemption, translation and selected Linux mapping implementation included. Memory means are arithmetic sample means, including phase boundaries; sampled peaks can miss short peaks. Resident lifetime peak is separate.',
              'sha256': input_hashes, 'runs': results}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'summary.json')
    if len(results) != args.repeat * 2 or any(not row['validated'] for row in results):
        raise SystemExit('Linux 翻译原型失败，查看对应 jsonl/log')


if __name__ == '__main__':
    main()
