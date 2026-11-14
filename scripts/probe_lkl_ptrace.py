#!/usr/bin/env python3
"""用同一静态 guest 检查原内核和候选内核的通用 ptrace 行为。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
from process_observer import observe

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
RUNTIME_FILES = ('bin/linker64', 'lib64/bionic/libc.so', 'lib64/bionic/libdl.so', 'lib64/bionic/libm.so')


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def dependency_inputs(path):
    text = path.read_text().replace('\\\n', ' ')
    return [Path(name).resolve() for name in shlex.split(text.split(':', 1)[1])]


def kernel_offsets(kernel):
    symbols = subprocess.check_output(['/opt/homebrew/opt/llvm/bin/llvm-nm', kernel / 'lkl.so'], text=True)
    elf = (kernel / 'lkl.so').read_bytes()
    image = (kernel / 'lkl-image.bin').read_bytes()
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', elf, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', elf, phoff + index * phsize) for index in range(phcount)]
    constants = {name: int(value) for name, value in re.findall(
        r'^#define ([A-Z_]+) ([0-9]+)$', (kernel / 'lkl-image.h').read_text(), re.M)}
    if constants.pop('LKL_IMAGE_SIZE') != len(image):
        raise ValueError('内核 ABI 头记录的镜像大小不同')
    exports = {name: name.lower() for name in constants}
    translation = {'TRANSLATION_COPY_USER': 'nce_copy_user', 'TRANSLATION_LOOKUP': 'nce_translate_user'}
    offsets = {}
    for constant, name in (exports | translation).items():
        match = re.search(r'^([0-9a-f]+) [tT] ' + name + '$', symbols, re.M)
        if not match:
            raise ValueError('内核缺少 ' + name)
        address = int(match[1], 16)
        if constant in constants and constants[constant] != address:
            raise ValueError('内核 ABI 头的函数偏移与 ELF 不同：' + name)
        if not any(kind == 1 and flags & 1 and start <= address and address + 64 <= start + size and
                   elf[offset + address - start:offset + address - start + 64] == image[address:address + 64]
                   for kind, flags, offset, start, _, size, _, _ in segments):
            raise ValueError('内核函数的 ELF 地址与镜像不同：' + name)
        offsets[constant] = {'symbol': name, 'offset': address, 'elf_image_bytes_checked': 64}
    header = ''.join(f'#define {constant} 0x{offsets[constant]["offset"]:x}UL\n' for constant in translation)
    return header, offsets


def environment(configuration, files):
    env = {key: value for key, value in os.environ.items() if not key.startswith('NCE_')}
    values = {'FILES': str(files), 'GUEST_LAYOUT': configuration['guest_layout'],
              'DATA_TBI': int(configuration['data_tbi']), 'ONLINE_CPUS': configuration['online_cpus'],
              'BATCH': configuration['instruction_batch'], 'PARALLEL_PROFILE': int(configuration['parallel_profile']),
              'UACCESS_STOP': configuration['uaccess_stop'], 'PREEMPTION': configuration['preemption'],
              'MEMORY_MS': configuration['memory_interval_ms'], 'WORKLOAD': 'bionic',
              'CACHE': configuration['cache_invalidation'], 'EXEC_CACHE': configuration['exec_cache'],
              'TLB': configuration['tlb_invalidation'], 'LINUX_FASTPATH': configuration['linux_fastpath'],
              'CACHE_MIB': configuration['translation_cache_bytes'] >> 20}
    env.update({'NCE_TRANSLATION_' + key: str(value) for key, value in values.items()})
    env.update({'NCE_DIRECT_' + key.upper(): str(int(value) if isinstance(value, bool) else value)
                for key, value in configuration['direct'].items()})
    env['NCE_SCHED_SLICE_US'] = '0'
    return env


def validate(rows, host, candidate):
    checks = [row for row in rows if row.get('test') == 'lkl_ptrace']
    complete = [row for row in rows if row.get('test') == 'lkl_ptrace_complete']
    reaped = [row for row in checks if row.get('step') == 'owned_children_reaped']
    cleanup = [row for row in rows if row.get('test') == 'translated_bpf_code_pool_cleanup']
    expected_failures = [row for row in checks if row.get('step') in ('interrupt', 'geteventmsg')]
    process_ok = (host['process_group_empty'] and not host['cleanup_signals'] and
                  not host['errors'] and not host['output_limit_exceeded'] and not host['timed_out'])
    cleanup_ok = len(reaped) == 3 and all(row['passed'] for row in reaped) and cleanup == [
        {'test': 'translated_bpf_code_pool_cleanup', 'passed': True}]
    if candidate:
        expected = (host['returncode'] == 0 and checks and all(row['passed'] for row in checks) and
                    complete == [{'test': 'lkl_ptrace_complete', 'passed': True}] and
                    rows[-1] == {'result': 'passed'})
    else:
        expected = (host['returncode'] == 1 and len(expected_failures) == 3 and
                    all(row['result'] == -1 and row['errno'] == 22 for row in expected_failures) and
                    all(row['passed'] for row in checks if row not in expected_failures) and
                    complete == [{'test': 'lkl_ptrace_complete', 'passed': False}] and
                    rows and rows[-1] == {'result': 'failed'})
    return {'status': 'passed' if process_ok and cleanup_ok and expected else 'failed',
            'host_process_passed': process_ok, 'guest_and_kernel_cleanup_passed': cleanup_ok,
            'expected_behavior_observed': bool(expected), 'checks': checks,
            'guest_complete': complete}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline_summary', type=Path)
    parser.add_argument('candidate_kernel', type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--baseline-build-revision',
                        help='原 core 构建脚本的固定 Git 提交，仅用于已更新的 probe_lkl.py')
    args = parser.parse_args()
    baseline_path = args.baseline_summary.resolve()
    baseline = json.loads(baseline_path.read_text())
    core = baseline_path.parent
    original = Path(baseline['kernel_candidate']['path'])
    original_repo = original.parents[2]
    candidate, out = args.candidate_kernel.resolve(), args.output.resolve()
    runtime = args.runtime.resolve()
    if out.exists():
        raise SystemExit('本轮 ptrace 输出目录已存在')
    configuration = baseline['configuration']
    if (configuration['cpus'], configuration['guest_page_bytes'], configuration['execution'],
            configuration['bpf_jit']) != (1, 16384, 'direct', True):
        raise SystemExit('本轮回归需要单 CPU 16 KiB Direct/BPF JIT 基线')
    out.mkdir(parents=True)
    files = out / 'files'
    files.mkdir()
    before = {}
    report = {'scope': 'generic-ptrace-cli-gcd', 'status': 'running', 'platform': platform.platform(),
              'machine': platform.machine(), 'configuration': configuration,
              'baseline_summary': str(baseline_path), 'baseline_summary_sha256': sha(baseline_path),
              'loader_workload': 'bionic', 'executed_guest': 'static-musl-ptrace-fixture',
              'commands': [], 'clones': [], 'host_dependencies': {}, 'kernel_exports': {},
              'baseline_source_history': [], 'runs': []}

    def track(path):
        path = Path(path).resolve()
        digest = sha(path)
        if str(path) in before and before[str(path)] != digest:
            raise ValueError('ptrace 输入摘要变化：' + str(path))
        before[str(path)] = digest

    def save():
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')

    def command(argv, log):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        subprocess.run(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)

    def clone(source, target):
        track(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        argv = ['cp', '-c', str(source), str(target)]
        subprocess.run(argv, check=True, timeout=30)
        if sha(source) != sha(target):
            raise ValueError('APFS 文件克隆摘要不同：' + str(target))
        report['clones'].append({'source': str(source), 'target': str(target), 'method': 'cp -c',
                                 'bytes': target.stat().st_size, 'sha256': sha(target)})
        track(target)

    try:
        track(baseline_path)
        track(Path(__file__))
        track(ROOT / 'scripts/process_observer.py')
        track(EXP / 'translation/ptrace-guest.c')
        track(ROOT / 'scripts/lkl_nce_patch.py')
        for name, digest in baseline['sha256'].items():
            path = original_repo / name
            actual = sha(path)
            if actual != digest:
                if name != 'scripts/probe_lkl.py' or not args.baseline_build_revision:
                    raise ValueError('固定 core 基线输入摘要不匹配：' + str(path))
                revision = subprocess.check_output(
                    ['git', 'rev-parse', args.baseline_build_revision + '^{commit}'], cwd=ROOT, text=True).strip()
                blob = subprocess.check_output(['git', 'show', revision + ':' + name], cwd=ROOT)
                if hashlib.sha256(blob).hexdigest() != digest:
                    raise ValueError('历史 core 构建脚本的 Git blob 摘要不匹配：' + name)
                report['baseline_source_history'].append({
                    'path': str(path), 'expected_sha256': digest, 'current_sha256': actual,
                    'source_revision': revision,
                    'source_blob': subprocess.check_output(
                        ['git', 'rev-parse', revision + ':' + name], cwd=ROOT, text=True).strip(),
                    'current_path_revision': subprocess.check_output(
                        ['git', 'log', '-1', '--format=%H', '--', name], cwd=original_repo, text=True).strip(),
                    'role': 'historical-kernel-build-script-not-executed-by-ptrace-host-build'})
            track(path)
        for kernel in (original, candidate):
            for name in ('lkl.so', 'lkl-image.bin', 'lkl-image.h', 'lkl-host-ops.h', 'build/.config'):
                track(kernel / name)
            for name in ('source/arch/lkl/include/asm/ptrace.h', 'source/arch/lkl/include/asm/processor.h',
                         'source/arch/lkl/kernel/threads.c', 'source/arch/lkl/kernel/nce_process.c',
                         'source/arch/lkl/kernel/nce_signal.c', 'source/kernel/ptrace.c',
                         'build/kernel/ptrace.o', 'build/kernel/.ptrace.o.cmd',
                         'build/arch/lkl/kernel/threads.o', 'build/arch/lkl/kernel/.threads.o.cmd',
                         'build/arch/lkl/kernel/nce_process.o', 'build/arch/lkl/kernel/.nce_process.o.cmd',
                         'build/arch/lkl/kernel/nce_signal.o', 'build/arch/lkl/kernel/.nce_signal.o.cmd'):
                track(kernel / name)
        if (original / 'build/.config').read_bytes() != (candidate / 'build/.config').read_bytes():
            raise ValueError('ptrace 候选与原内核配置不同')
        libc = original_repo / 'work/native-linux-nce/musl-build/install'
        builtins = original_repo / 'work/native-linux-nce/compiler-rt-build/libbuiltins.a'
        llvm = Path('/opt/homebrew/opt/llvm/bin')
        resource = Path(subprocess.check_output([llvm / 'clang', '-print-resource-dir'], text=True).strip()) / 'include'
        with (out / 'build.log').open('w') as log:
            command([llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
                     '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
                     '-MD', '-MF', out / 'guest.d', '-c', EXP / 'translation/ptrace-guest.c', '-o', out / 'guest.o'], log)
            for path in [*dependency_inputs(out / 'guest.d'), builtins, *[libc / 'lib' / name for name in
                         ('rcrt1.o', 'crti.o', 'libc.a', 'crtn.o')]]:
                track(path)
            command([shutil.which('ld.lld'), '-pie', '--gc-sections', '--no-dynamic-linker', '-z', 'max-page-size=16384',
                     '-z', 'common-page-size=16384', libc / 'lib/rcrt1.o', libc / 'lib/crti.o', out / 'guest.o',
                     libc / 'lib/libc.a', builtins, libc / 'lib/crtn.o', '-o', files / 'guest.elf'], log)
            track(files / 'guest.elf')
            for name in ('sigreturn.elf', 'dynamic.elf', 'dash.elf', 'bionic-test.elf', 'libc.so', 'dependency.so', 'plugin.so'):
                clone(core / name, files / name)
            clone(files / 'guest.elf', files / 'late.elf')
            for name in RUNTIME_FILES:
                clone(runtime / name, files / 'stock-runtime' / name)
            config = files / 'stock-ld.config.txt'
            config.write_text('dir.stock = /system/bin\n\n[stock]\nnamespace.default.isolated = false\n'
                              'namespace.default.search.paths = /apex/com.android.runtime/lib64/bionic\n')
            track(config)
            for label, kernel in (('baseline', original), ('candidate', candidate)):
                hosts = out / label
                hosts.mkdir()
                header, report['kernel_exports'][label] = kernel_offsets(kernel)
                if label == 'baseline':
                    track(core / 'translation-symbols.h')
                    if header != (core / 'translation-symbols.h').read_text():
                        raise ValueError('原基线 translation 函数偏移与 ELF 不同')
                (hosts / 'translation-symbols.h').write_text(header)
                track(hosts / 'translation-symbols.h')
                for original_command in baseline['commands'][:25]:
                    argv = []
                    for value in original_command:
                        value = value.replace(str(original_repo / 'experiments'), str(ROOT / 'experiments'))
                        if value == str(original):
                            value = str(kernel)
                        if value == str(core) or (value.startswith(str(core) + '/') and
                                                  not value.startswith(str(core / 'direct') + '/')):
                            value = value.replace(str(core), str(hosts), 1)
                        argv.append(value)
                    for index, value in enumerate(argv):
                        if value == '-c':
                            track(argv[index + 1])
                    command(argv, log)
                dependencies = sorted(hosts.glob('*-host-*.d'))
                if len(dependencies) != 23:
                    raise ValueError('ptrace 宿主编译依赖数量不同')
                actual = sorted(set(path for dependency in dependencies for path in dependency_inputs(dependency)))
                if (kernel / 'lkl-image.h').resolve() not in actual or (original / 'lkl-image.h').resolve() in actual and label == 'candidate':
                    raise ValueError('ptrace 宿主实际内核头依赖不同')
                if (hosts / 'translation-symbols.h').resolve() not in actual:
                    raise ValueError('ptrace 宿主未使用当前内核的 translation 函数偏移')
                for path in actual:
                    track(path)
                report['host_dependencies'][label] = [{'path': str(path), 'sha256': sha(path)} for path in actual]
                for entry in ('cli', 'gcd'):
                    track(hosts / entry)
                print('built', label, 'CLI/GCD', flush=True)
        report['guest_sha256'] = sha(files / 'guest.elf')
        env = environment(configuration, files)
        report['runtime_environment'] = {key: value for key, value in env.items() if key.startswith('NCE_')}
        save()
        for label, kernel in (('baseline', original), ('candidate', candidate)):
            for entry in ('cli', 'gcd'):
                stem = label + '-' + entry
                stdout, stderr = out / (stem + '.jsonl'), out / (stem + '.stderr')
                argv = [str(out / label / entry), str(kernel / 'lkl-image.bin')]
                with stdout.open('xb') as output, stderr.open('xb') as error:
                    host = observe(argv, env, output, error, 60)
                try:
                    rows = [json.loads(line) for line in stdout.read_text().splitlines() if line.startswith('{')]
                    result = validate(rows, host, label == 'candidate')
                except json.JSONDecodeError as error:
                    result = {'status': 'failed', 'checks': [],
                              'error': {'type': type(error).__name__, 'message': str(error)}}
                report['runs'].append({'kernel': label, 'entry': entry, 'command': argv, 'host_process': host,
                                       'raw_stdout': str(stdout), 'raw_stdout_sha256': sha(stdout),
                                       'raw_stderr': str(stderr), 'raw_stderr_sha256': sha(stderr),
                                       'validation': result})
                save()
                print(stem, result['status'], 'host_exit', host['returncode'], flush=True)
        report['status'] = 'passed' if all(run['validation']['status'] == 'passed' for run in report['runs']) else 'failed'
    except BaseException as error:
        report.update(status='failed', error={'type': type(error).__name__, 'message': str(error)})
    finally:
        report['inputs_before'] = before
        report['inputs_after'] = {path: sha(path) if Path(path).is_file() else None for path in before}
        report['inputs_unchanged'] = report['inputs_before'] == report['inputs_after']
        if not report['inputs_unchanged']:
            report['status'] = 'failed'
        save()
    print(json.dumps({'status': report['status'], 'inputs_unchanged': report['inputs_unchanged'],
                      'error': report.get('error')}, ensure_ascii=False))
    raise SystemExit(0 if report['status'] == 'passed' else 1)


if __name__ == '__main__':
    main()
