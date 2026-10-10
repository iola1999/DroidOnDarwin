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


def validate(rows, memory_interval):
    expected = {'linux_dynamic_code', 'linux_signal_suite', 'linux_signal_register_restore',
                'linux_signal_eintr_restart', 'linux_signal_timer_preemption', 'linux_mm_exchange_integrity',
                'translated_pthread_tls_mutex', 'translated_signals_protection_restore',
                'translated_runtime_elf_exec', 'translated_runtime_libraries', 'translated_context_cleanup',
                'linux_dynamic_link_dlopen_constructors', 'linux_dynamic_tls_threads', 'linux_dynamic_fork_signal'}
    if not rows or rows[-1].get('result') != 'passed' or any(row.get('passed') is False for row in rows):
        return False
    if not expected <= {row.get('test') for row in rows if row.get('passed')}:
        return False
    cases = [(row['bytes'], row['shared_percent'], row['touch_all'], row['threads'])
             for row in rows if row.get('bench') == 'linux_mm_exchange']
    expected_cases = {(size * 1024 * 1024, shared, touch, 0)
                      for size in (2, 4, 8, 16) for shared in (0, 50, 100) for touch in (0, 1)}
    expected_cases.update((size * 1024 * 1024, 0, 1, 1) for size in (2, 4, 8, 16))
    if len(cases) != 28 or set(cases) != expected_cases:
        return False
    phases = ('kernel_setup', 'guest_setup', 'guest_execution', 'cleanup')
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
    dependency = json.loads(subprocess.check_output([WORK / 'user-translation-venv/bin/python', '-c',
        'import json,pathlib,unicorn; print(json.dumps({"version":unicorn.__version__,"path":str(pathlib.Path(unicorn.__file__).parent)}))'], text=True))
    if dependency['version'] != '2.1.4':
        parser.error('该实验固定使用 unicorn==2.1.4')
    library = Path(dependency['path'])
    out.mkdir(parents=True, exist_ok=True)
    llvm = Path('/opt/homebrew/opt/llvm/bin')
    symbols = subprocess.check_output([llvm / 'llvm-nm', KERNEL / 'lkl.so'], text=True)
    match = re.search(r'^([0-9a-f]+) t nce_copy_user$', symbols, re.M)
    if not match:
        parser.error('当前单核内核缺少 nce_copy_user')
    native_manifest = json.loads((KERNEL / 'lkl-summary.json').read_text())
    if sha(KERNEL / 'lkl-image.bin') != native_manifest['kernel_sha256'] or any(
        sha(KERNEL / name) != native_manifest['generated_sha256'][name] for name in ('lkl-image.h', 'lkl-host-ops.h')):
        parser.error('内核镜像或头文件与本机验证记录不一致，请先重新构建并验证单核内核')
    elf = (KERNEL / 'lkl.so').read_bytes()
    image = (KERNEL / 'lkl-image.bin').read_bytes()
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', elf, 54)
    symbol = int(match[1], 16)
    matched = False
    for index in range(phcount):
        kind, flags, offset, address, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + index * phsize)
        if kind == 1 and flags & 1 and address <= symbol and symbol + 64 <= address + size:
            matched = elf[offset + symbol - address:offset + symbol - address + 64] == image[symbol:symbol + 64]
    if not matched:
        parser.error('nce_copy_user 的 ELF 地址与内核镜像不一致')
    (out / 'translation-symbols.h').write_text(f'#define TRANSLATION_COPY_USER 0x{match[1]}UL\n')
    inputs = [EXP / 'translation/linux-execution.c', EXP / 'translation/linux-runner.c', EXP / 'translation/linux-memory.c', EXP / 'lkl-host.c']
    commands = []
    for mode in ('cli', 'gcd'):
        command = ['xcrun', 'clang', '-O2', '-g', '-DNCE_PROCESS', '-Wall', '-Wextra', '-Werror',
                   '-I', EXP, '-I', KERNEL, '-I', out, '-I', KERNEL / 'host-include', '-I', library / 'include',
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
    for name in ('signal-tests.c', 'signal-registers.S', 'code-tests.c', 'mm-bench.c'):
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
    runtime_files = ('sigreturn.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so')
    for name in runtime_files:
        shutil.copyfile(KERNEL / name, out / name)
    if hosts_before != {mode: sha(out / mode) for mode in hosts_before}:
        raise RuntimeError('客体构建后宿主文件发生变化')
    files = [*inputs, EXP / 'translation/linux-execution.h', EXP / 'translation/linux-memory.h', EXP / 'translation/linux-guest.c', Path(__file__),
             EXP / 'dispatch-test.c', EXP / 'layout.h', EXP / 'guest-registers.h',
             *[EXP / 'userspace' / name for name in ('signal-tests.c', 'signal-registers.S', 'code-tests.c', 'mm-bench.c')],
             KERNEL / 'lkl-image.bin', KERNEL / 'lkl-image.h', KERNEL / 'lkl.so', KERNEL / 'lkl-host-ops.h',
             library / 'lib/libunicorn.a', *[out / name for name in ('cli', 'gcd', 'guest.elf', 'late.elf', *runtime_files)]]
    input_hashes = {str(path.relative_to(ROOT)): sha(path) for path in files}
    if args.build_only:
        print(out)
        return
    environment = os.environ.copy()
    environment['NCE_TRANSLATION_FILES'] = str(out)
    environment['NCE_TRANSLATION_BATCH'] = str(args.batch_instructions)
    environment['NCE_TRANSLATION_PREEMPTION'] = args.preemption
    environment['NCE_TRANSLATION_MEMORY_MS'] = str(args.memory_interval_ms)
    results = []
    for iteration in range(args.repeat):
        for mode in ('cli', 'gcd'):
            stem = f'{mode}-{iteration}'
            with (out / f'{stem}.jsonl').open('w') as output, (out / f'{stem}.log').open('w') as error:
                try:
                    process = subprocess.run([out / mode, KERNEL / 'lkl-image.bin'], env=environment,
                                             stdout=output, stderr=error, timeout=120)
                    returncode = process.returncode
                except subprocess.TimeoutExpired:
                    returncode = 'timeout'
            rows = [json.loads(line) for line in (out / f'{stem}.jsonl').read_text().splitlines() if line.startswith('{')]
            passed = returncode == 0 and validate(rows, args.memory_interval_ms)
            results.append({'entry': mode, 'iteration': iteration, 'returncode': returncode, 'results': rows,
                            'validated': passed,
                            'stdout_sha256': sha(out / f'{stem}.jsonl'), 'stderr_sha256': sha(out / f'{stem}.log')})
            if not passed:
                break
        if not results[-1]['validated']:
            break
    if input_hashes != {str(path.relative_to(ROOT)): sha(path) for path in files}:
        raise SystemExit('本轮验证期间源码或二进制发生变化，请重跑')
    report = {'scope': 'Native single-CPU LKL with translated ARM64 userspace; current window bookkeeping retained; no Android/iPhone integration',
              'platform': platform.platform(), 'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'dependency': dependency, 'commands': commands, 'host_unchanged_after_guest_build': True,
              'configuration': {'mapping': 'demand', 'sparse_scan': True, 'fault_prefetch': False, 'guest_page_bytes': 16384,
                                'guest_ram_bytes': 1073741824, 'translation_cache_bytes': 16777216,
                                'instruction_batch': args.batch_instructions, 'memory_interval_ms': args.memory_interval_ms,
                                'preemption': args.preemption, 'timer_interval_ns': 2000000},
              'measurement': 'Guest clock times 128 pipe round trips per memory case; excludes initial COW materialization. Selected preemption, translation and Linux window bookkeeping included. Memory means are arithmetic sample means, including phase boundaries; sampled peaks can miss short peaks. Resident lifetime peak is separate.',
              'sha256': input_hashes, 'runs': results}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'summary.json')
    if len(results) != args.repeat * 2 or any(not row['validated'] for row in results):
        raise SystemExit('Linux 翻译原型失败，查看对应 jsonl/log')


if __name__ == '__main__':
    main()
