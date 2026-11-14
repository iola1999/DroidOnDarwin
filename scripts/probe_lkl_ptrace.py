#!/usr/bin/env python3
"""构建同一静态 guest，分别验证通用 ptrace 和 ARM64 只读 regset。"""
import argparse
from collections import Counter
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
                for key, value in (configuration['direct'] or {}).items()})
    env['NCE_SCHED_SLICE_US'] = '0'
    return env


def expected_regset_checks(supported):
    # ptrace-guest.c 的已完成路径包含重复读取；相同 case/step 的次数也属于检查范围。
    counts = Counter()

    def add(case, steps, repetitions=1):
        counts.update((case, step) for _ in range(repetitions) for step in steps)

    add('request_checks', ('invalid_target', 'seize_self'))
    interrupt_steps = (
        'child_ready', 'untraced_target', 'seize', 'interrupt', 'interrupt_stop',
        'stopped_target_idle', 'regset_reads_kept_target_stopped', 'continue',
        'continued_target', 'interrupt_again', 'interrupt_stop_again', 'detach',
        'owned_children_reaped')
    for case in ('running_user_loop', 'blocking_read'):
        add(case, interrupt_steps)
    add('running_user_loop', ('loop_started', 'detached_target_running'))
    add('blocking_read', ('read_blocked', 'user_read_return', 'blocking_read_tls_restored',
                          'blocking_read_restarted'))
    clone_steps = (
        'child_ready', 'seize_traceclone', 'clone_event_stop', 'geteventmsg',
        'actual_clone_pid', 'geteventmsg_invalid_output', 'clone_child_stop',
        'clone_stopped_before_user_write', 'queue_blocked_clone_signal',
        'continue_clone_child', 'clone_child_exit', 'detach_clone_parent',
        'real_parent_waited_clone', 'clone_tls_stack_frame_preserved',
        'clone_signal_mask_preserved', 'clone_blocked_signal_pending',
        'clone_parent_tls_preserved', 'owned_children_reaped')
    for case in ('clone_event', 'clone_settls'):
        add(case, clone_steps)
    add('clone_settls', (
        'clone_requested_tls_and_raw_state_preserved', 'clone_resumed_with_requested_tls',
        'interrupt_clone_child', 'clone_child_second_stop', 'continue_clone_child_again'))

    read_steps = (
        'getregset_prstatus', 'getregset_prstatus', 'getregset_tls_full',
        'getregset_tls_full_value', 'getregset_tls_short', 'getregset_tls_short_value',
        'readonly_regset_target_preserved')
    for case, repetitions in (('running_user_loop', 2), ('blocking_read', 2),
                              ('clone_event', 2), ('clone_settls', 3)):
        add(case, read_steps, repetitions)
    for note, full_bytes in ((1, 272), (0x401, 16)):
        for length in (0, 8, full_bytes, full_bytes + 8, (1 << 64) - 8, 1, 9, full_bytes + 1):
            step = f'getregset_{note:x}_length_{length}'
            add('running_user_loop', (step, step + '_output'))
        add('running_user_loop', tuple(f'getregset_{note:x}_{suffix}' for suffix in (
            'invalid_iovec', 'invalid_output', 'crossing_output', 'crossing_output_range',
            'crossing_iovec', 'readonly_iov_len', 'readonly_iov_len_output')) +
            ('invalid_output_iovec_preserved', 'invalid_iovec_output_preserved'))
    for note in (1, 0x401, 2, 0x409, 0x40a, 0xdead):
        add('running_user_loop', (f'setregset_{note:x}_invalid_iovec',))
    for note in (2, 0x409, 0x40a, 0xdead):
        add('running_user_loop', (f'getregset_{note:x}_invalid_iovec',))

    if supported:
        snapshot_steps = ('gpr_matches_assembly_snapshot', 'raw_sp_eight_byte_offset_preserved')
        for case in ('running_user_loop', 'blocking_read', 'clone_settls'):
            add(case, snapshot_steps, 2)
        for case in ('running_user_loop', 'blocking_read'):
            add(case, ('assembly_snapshot_ready',), 2)
        add('running_user_loop', ('stopped_pc_in_raw_user_loop',), 2)
    return counts


def validate(rows, host, candidate, expected_regset=None, execution='direct'):
    checks = [row for row in rows if row.get('test') == 'lkl_ptrace']
    actual_counts = Counter((row.get('case'), row.get('step')) for row in checks)
    expected_counts = expected_regset_checks(expected_regset) if expected_regset is not None else None
    checks_complete = expected_counts is None or actual_counts == expected_counts
    check_completeness = {
        'status': 'not_checked' if expected_counts is None else 'passed' if checks_complete else 'failed',
        'expected_count': sum(expected_counts.values()) if expected_counts is not None else None,
        'actual_count': len(checks),
        'missing': [{'case': case, 'step': step, 'count': count}
                    for (case, step), count in (expected_counts - actual_counts).items()]
        if expected_counts is not None else [],
        'unexpected': [{'case': case, 'step': step, 'count': count}
                       for (case, step), count in (actual_counts - expected_counts).items()]
        if expected_counts is not None else []}
    complete = [row for row in rows if row.get('test') == 'lkl_ptrace_complete']
    reaped = [row for row in checks if row.get('step') == 'owned_children_reaped']
    cleanup = [row for row in rows if row.get('test') == 'translated_bpf_code_pool_cleanup']
    expected_failures = [row for row in checks if row.get('step') in ('interrupt', 'geteventmsg')]
    process_ok = (host['process_group_empty'] and not host['cleanup_signals'] and
                  not host['errors'] and not host['output_limit_exceeded'] and not host['timed_out'])
    cleanup_ok = ({row.get('case') for row in reaped} == {
        'running_user_loop', 'blocking_read', 'clone_event', 'clone_settls'} and
        len(reaped) == 4 and all(row['passed'] for row in reaped) and cleanup == [
        {'test': 'translated_bpf_code_pool_cleanup', 'passed': True}]
    )
    if candidate:
        expected = (host['returncode'] == 0 and checks and all(row['passed'] for row in checks) and
                    complete == [{'test': 'lkl_ptrace_complete', 'passed': True}] and
                    rows[-1] == {'result': 'passed'})
    else:
        expected = (host['returncode'] == 1 and len(expected_failures) == 4 and
                    all(row['result'] == -1 and row['errno'] == 22 for row in expected_failures) and
                    all(row['passed'] for row in checks if row not in expected_failures) and
                    complete == [{'test': 'lkl_ptrace_complete', 'passed': False}] and
                    rows and rows[-1] == {'result': 'failed'})
    capabilities = [row for row in rows if row.get('test') == 'lkl_ptrace_regset_capability']
    snapshots = [row for row in rows if row.get('test') == 'lkl_ptrace_register_snapshot']
    regset_ok = (expected_regset is None or capabilities == [
        {'test': 'lkl_ptrace_regset_capability', 'supported': expected_regset}])
    if expected_regset:
        regset_ok = regset_ok and sorted((row.get('case'), row.get('mode')) for row in snapshots) == sorted(
            [('running_user_loop', 1)] * 2 + [('blocking_read', 2)] * 2 +
            [('clone_settls', 3), ('clone_settls', 4)])
        for row in snapshots:
            actual, wanted, mask = row.get('actual', []), row.get('expected', []), row.get('comparison_mask', 0)
            expected_mask = ((1 << 34) - 1 if row.get('mode') in (1, 2) else
                             sum(1 << index for index in (0, 18, 29, 31, 32, 33)))
            regset_ok = (regset_ok and len(actual) == len(wanted) == 34 and row.get('passed') is True and
                         mask == expected_mask and row.get('differences') == 0 and all(a == b for index, (a, b) in
                             enumerate(zip(actual, wanted)) if mask & (1 << index)))
    elif expected_regset is False:
        regset_ok = regset_ok and not snapshots
    direct = [row for row in rows if row.get('direct_execution')]
    execution_ok = (bool(direct) and any(row.get('calls', 0) > 0 for row in direct)
                    if execution == 'direct' else not direct)
    return {'status': 'passed' if process_ok and cleanup_ok and expected and regset_ok and execution_ok and
            checks_complete else 'failed',
            'host_process_passed': process_ok, 'guest_and_kernel_cleanup_passed': cleanup_ok,
            'expected_behavior_observed': bool(expected), 'checks': checks,
            'check_completeness': check_completeness,
            'guest_complete': complete, 'regset_capability': capabilities,
            'regset_content_passed': bool(regset_ok), 'register_snapshots': snapshots,
            'execution_passed': execution_ok}


def run_prepared(prepared_path):
    prepared_path = prepared_path.resolve()
    prepared_digest = sha(prepared_path)
    prepared = json.loads(prepared_path.read_text())
    if prepared.get('status') != 'prepared' or not prepared.get('inputs_unchanged'):
        raise ValueError('ptrace 准备记录未完成输入检查')
    before = dict(prepared['inputs_before'])
    if before != prepared['inputs_after']:
        raise ValueError('ptrace 准备记录的前后输入不同')
    actual = {path: sha(path) if Path(path).is_file() else None for path in before}
    if actual != before:
        changed = [path for path in before if actual[path] != before[path]]
        raise ValueError('ptrace 运行输入与准备记录不同：' + ', '.join(changed))
    out = Path(prepared['output'])
    if prepared_path != out / 'prepared.json':
        raise ValueError('ptrace 准备记录的输出路径不同')
    run_out = out / 'run'
    run_out.mkdir()
    before[str(prepared_path)] = prepared_digest
    report = {**prepared, 'status': 'running', 'prepared_result': str(prepared_path),
              'prepared_sha256': prepared_digest, 'runs': [], 'inputs_before': before}
    del report['guest_started']
    report_path = run_out / 'report.json'

    def save():
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')

    try:
        for plan in prepared['run_plan']:
            stem = plan['label'] + '-' + plan['entry']
            stdout, stderr = run_out / (stem + '.jsonl'), run_out / (stem + '.stderr')
            env = environment(plan['configuration'], Path(prepared['files']))
            with stdout.open('xb') as output, stderr.open('xb') as error:
                host = observe(plan['command'], env, output, error, 60)
            try:
                rows = [json.loads(line) for line in stdout.read_text().splitlines() if line.startswith('{')]
                result = validate(rows, host, plan['generic_ptrace_supported'], plan['regset_supported'],
                                  plan['configuration']['execution'])
            except json.JSONDecodeError as error:
                result = {'status': 'failed', 'checks': [],
                          'error': {'type': type(error).__name__, 'message': str(error)}}
            report['runs'].append({
                'kernel': plan['label'], 'entry': plan['entry'], 'command': plan['command'],
                'configuration': plan['configuration'], 'guest_sha256': prepared['guest_sha256'],
                'runtime_environment': {key: value for key, value in env.items() if key.startswith('NCE_')},
                'host_process': host, 'raw_stdout': str(stdout), 'raw_stdout_sha256': sha(stdout),
                'raw_stderr': str(stderr), 'raw_stderr_sha256': sha(stderr), 'validation': result})
            save()
            print(stem, result['status'], 'host_exit', host['returncode'], flush=True)
        report['status'] = 'passed' if report['runs'] and all(
            run['validation']['status'] == 'passed' for run in report['runs']) else 'failed'
    except BaseException as error:
        report.update(status='failed', error={'type': type(error).__name__, 'message': str(error)})
    finally:
        report['inputs_after'] = {path: sha(path) if Path(path).is_file() else None for path in before}
        report['inputs_unchanged'] = report['inputs_before'] == report['inputs_after']
        if not report['inputs_unchanged']:
            report['status'] = 'failed'
        save()
    print(json.dumps({'status': report['status'], 'report': str(report_path),
                      'inputs_unchanged': report['inputs_unchanged'], 'error': report.get('error')}, ensure_ascii=False))
    return report['status'] == 'passed'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline_summary', nargs='?', type=Path)
    parser.add_argument('candidate_kernel', nargs='?', type=Path)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--baseline-build-revision',
                        help='原 core 构建脚本的固定 Git 提交，仅用于已更新的 probe_lkl.py')
    parser.add_argument('--baseline-ptrace-report', type=Path,
                        help='核对并复用已交付通用 ptrace 报告中的候选 CLI/GCD，比较 ARM64 regset')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--build-only', action='store_true')
    mode.add_argument('--run-prepared', type=Path)
    args = parser.parse_args()
    if args.run_prepared:
        raise SystemExit(0 if run_prepared(args.run_prepared) else 1)
    if not all((args.baseline_summary, args.candidate_kernel, args.runtime, args.output)):
        parser.error('构建需要 baseline_summary、candidate_kernel、--runtime 和 --output')
    baseline_path = args.baseline_summary.resolve()
    baseline = json.loads(baseline_path.read_text())
    core = baseline_path.parent
    core_kernel = Path(baseline['kernel_candidate']['path'])
    original_repo = core_kernel.parents[2]
    original = core_kernel
    ptrace_path = args.baseline_ptrace_report.resolve() if args.baseline_ptrace_report else None
    ptrace_baseline = json.loads(ptrace_path.read_text()) if ptrace_path else None
    original_hosts = None
    if ptrace_baseline:
        runs = [run for run in ptrace_baseline['runs'] if run['kernel'] == 'candidate']
        if (ptrace_baseline['status'] != 'passed' or not ptrace_baseline['inputs_unchanged'] or
                ptrace_baseline['configuration'] != baseline['configuration'] or
                ptrace_baseline['baseline_summary'] != str(baseline_path) or
                ptrace_baseline['baseline_summary_sha256'] != sha(baseline_path) or
                len(runs) != 2 or {run['entry'] for run in runs} != {'cli', 'gcd'} or
                any(run['validation']['status'] != 'passed' for run in runs) or
                len({str(Path(run['command'][1]).parent) for run in runs}) != 1 or
                len({str(Path(run['command'][0]).parent) for run in runs}) != 1):
            raise ValueError('原通用 ptrace 报告的候选来源或结果不同')
        original = Path(runs[0]['command'][1]).parent
        original_hosts = Path(runs[0]['command'][0]).parent
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
    report = {'scope': 'arm64-regset-read-cli-gcd' if ptrace_baseline else 'generic-ptrace-cli-gcd',
              'status': 'building', 'platform': platform.platform(),
              'machine': platform.machine(), 'configuration': configuration,
              'output': str(out), 'files': str(files), 'guest_started': False,
              'baseline_summary': str(baseline_path), 'baseline_summary_sha256': sha(baseline_path),
              'loader_workload': 'bionic', 'executed_guest': 'static-musl-ptrace-fixture',
              'commands': [], 'clones': [], 'host_dependencies': {}, 'kernel_exports': {},
              'host_dependency_manifests': {}, 'baseline_source_history': [], 'run_plan': [], 'runs': []}

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
        generated = {argv[index + 1] for index, value in enumerate(argv[:-1]) if value in ('-o', '-MF')}
        for value in argv:
            path = Path(value)
            if path.is_absolute() and path.is_file() and value not in generated:
                track(path)
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
        if ptrace_baseline:
            track(ptrace_path)
            expected = {row['path']: row['sha256'] for row in ptrace_baseline['host_dependencies']['candidate']}
            dependencies = sorted(original_hosts.glob('*-host-*.d'))
            actual = {str(path) for dependency in dependencies for path in dependency_inputs(dependency)}
            if len(dependencies) != 23 or actual != set(expected):
                raise ValueError('原通用 ptrace 的实际宿主依赖与报告不同')
            for path, digest in expected.items():
                if sha(path) != digest:
                    raise ValueError('原通用 ptrace 宿主依赖摘要不同：' + path)
                track(path)
            for path in [*dependencies, *[original_hosts / entry for entry in ('cli', 'gcd')],
                         original_hosts / 'translation-symbols.h',
                         *[original / name for name in ('lkl.so', 'lkl-image.bin', 'lkl-image.h', 'lkl-host-ops.h', 'build/.config')]]:
                expected_digest = ptrace_baseline['inputs_before'].get(str(path))
                if path not in dependencies and (expected_digest is None or sha(path) != expected_digest):
                    raise ValueError('原通用 ptrace 二进制或内核摘要不同：' + str(path))
                track(path)
            header, offsets = kernel_offsets(original)
            if offsets != ptrace_baseline['kernel_exports']['candidate'] or header != (original_hosts / 'translation-symbols.h').read_text():
                raise ValueError('原通用 ptrace ELF/image/ABI 关联不同')
            runner = [Path(path) for path in expected if path.endswith('/translation/linux-runner.c')]
            if len(runner) != 1 or not all(text in runner[0].read_text() for text in (
                    'getenv("NCE_TRANSLATION_FILES")', 'put_file("/probe-guest", "guest.elf")',
                    'put_file("/probe-after-build", "late.elf")')):
                raise ValueError('原通用 ptrace 宿主未按外部 files 加载本轮 fixture')
            report['baseline_reuse'] = {
                'report': str(ptrace_path), 'report_sha256': sha(ptrace_path), 'kernel': str(original),
                'host_dependencies_verified': True, 'actual_dependency_count': len(actual),
                'dependency_manifest_count': len(dependencies), 'binaries_verified': True,
                'kernel_elf_image_abi_verified': True,
                'external_fixture_loader': {'source': str(runner[0]), 'sha256': sha(runner[0]),
                                            'environment': 'NCE_TRANSLATION_FILES', 'files': ['guest.elf', 'late.elf']}}
        for name, digest in (baseline['sha256'].items() if not ptrace_baseline else []):
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
            if (kernel / 'source/arch/lkl/kernel/nce_ptrace.c').is_file():
                for name in ('source/arch/lkl/kernel/nce_ptrace.c', 'source/kernel/regset.c',
                             'build/arch/lkl/kernel/nce_ptrace.o', 'build/arch/lkl/kernel/.nce_ptrace.o.cmd',
                             'build/kernel/regset.o', 'build/kernel/.regset.o.cmd'):
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
        for path in (llvm / 'clang', Path(shutil.which('ld.lld')), Path(subprocess.check_output(
                ['xcrun', '--find', 'clang'], text=True).strip())):
            track(path)
        variants = [('baseline', original, configuration), ('candidate', candidate, configuration)]
        if ptrace_baseline:
            unicorn_configuration = {**configuration, 'execution': 'unicorn', 'guest_layout': 'host', 'direct': None}
            variants.append(('candidate-unicorn', candidate, unicorn_configuration))
            report['validation_scope'] = {
                'regsets': ['NT_PRSTATUS', 'NT_ARM_TLS'], 'fixture_bytes_shared': True,
                'direct_guest_layout': configuration['guest_layout'], 'unicorn_guest_layout': 'host',
                'performance_comparison': 'not_performed', 'native_regset_fixture': 'not_run',
                'sdk_crash_dump': 'not_run', 'ios_execution': 'not_run'}
            track(ROOT / 'scripts/probe_lkl_translation.py')
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
            track(out / 'guest.d')
            guest_elf = (files / 'guest.elf').read_bytes()
            if (guest_elf[:7] != b'\x7fELF\x02\x01\x01' or
                    struct.unpack_from('<HH', guest_elf, 16) != (3, 183)):
                raise ValueError('ptrace fixture 未生成 AArch64 ELF64 PIE')
            for name in ('sigreturn.elf', 'dynamic.elf', 'dash.elf', 'bionic-test.elf', 'libc.so', 'dependency.so', 'plugin.so'):
                clone(core / name, files / name)
            clone(files / 'guest.elf', files / 'late.elf')
            for name in RUNTIME_FILES:
                clone(runtime / name, files / 'stock-runtime' / name)
            config = files / 'stock-ld.config.txt'
            config.write_text('dir.stock = /system/bin\n\n[stock]\nnamespace.default.isolated = false\n'
                              'namespace.default.search.paths = /apex/com.android.runtime/lib64/bionic\n')
            track(config)
            for label, kernel, selected_configuration in variants:
                hosts = out / label
                hosts.mkdir()
                header, report['kernel_exports'][label] = kernel_offsets(kernel)
                if label == 'baseline' and ptrace_baseline:
                    clone(original_hosts / 'translation-symbols.h', hosts / 'translation-symbols.h')
                    for entry in ('cli', 'gcd'):
                        clone(original_hosts / entry, hosts / entry)
                    report['host_dependencies'][label] = ptrace_baseline['host_dependencies']['candidate']
                    report['host_dependency_manifests'][label] = [
                        {'path': str(path), 'sha256': sha(path)} for path in sorted(original_hosts.glob('*-host-*.d'))]
                    print('reused', label, 'CLI/GCD', flush=True)
                    continue
                if label == 'baseline':
                    track(core / 'translation-symbols.h')
                    if header != (core / 'translation-symbols.h').read_text():
                        raise ValueError('原基线 translation 函数偏移与 ELF 不同')
                (hosts / 'translation-symbols.h').write_text(header)
                track(hosts / 'translation-symbols.h')
                for original_command in baseline['commands'][:25]:
                    argv = []
                    for value in original_command:
                        if selected_configuration['execution'] == 'unicorn' and (
                                value in ('-DNCE_DIRECT_EXECUTION', '-lc++') or
                                value.startswith(str(core / 'direct') + '/')):
                            continue
                        value = value.replace(str(original_repo / 'experiments'), str(ROOT / 'experiments'))
                        if selected_configuration['execution'] == 'unicorn':
                            value = {str(EXP / 'translation/code-watch.c'): str(EXP / 'code-memory.c'),
                                     str(EXP / 'translation/read-view.c'): str(EXP / 'execute-check.c')}.get(value, value)
                        if value == str(core_kernel):
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
                for name in ('lkl-image.h', 'lkl-host-ops.h'):
                    if (kernel / name).resolve() not in actual or (
                            label != 'baseline' and (original / name).resolve() in actual):
                        raise ValueError('ptrace 宿主实际内核头依赖不同：' + name)
                if (hosts / 'translation-symbols.h').resolve() not in actual:
                    raise ValueError('ptrace 宿主未使用当前内核的 translation 函数偏移')
                for path in actual:
                    track(path)
                for path in dependencies:
                    track(path)
                report['host_dependencies'][label] = [{'path': str(path), 'sha256': sha(path)} for path in actual]
                report['host_dependency_manifests'][label] = [{'path': str(path), 'sha256': sha(path)} for path in dependencies]
                for entry in ('cli', 'gcd'):
                    track(hosts / entry)
                print('built', label, 'CLI/GCD', flush=True)
        report['guest_sha256'] = sha(files / 'guest.elf')
        report['fixture_source_sha256'] = sha(EXP / 'translation/ptrace-guest.c')
        report['fixture_dependency_manifest'] = {'path': str(out / 'guest.d'), 'sha256': sha(out / 'guest.d')}
        if sha(files / 'late.elf') != report['guest_sha256']:
            raise ValueError('ptrace guest.elf 与 late.elf 字节不同')
        env = environment(configuration, files)
        report['runtime_environment'] = {key: value for key, value in env.items() if key.startswith('NCE_')}
        for label, kernel, selected_configuration in variants:
            for entry in ('cli', 'gcd'):
                report['run_plan'].append({
                    'label': label, 'entry': entry, 'configuration': selected_configuration,
                    'command': [str(out / label / entry), str(kernel / 'lkl-image.bin')],
                    'generic_ptrace_supported': bool(ptrace_baseline) or label != 'baseline',
                    'regset_supported': label != 'baseline' if ptrace_baseline else None})
        report['status'] = 'prepared'
    except BaseException as error:
        report.update(status='failed', error={'type': type(error).__name__, 'message': str(error)})
    finally:
        report['inputs_before'] = before
        report['inputs_after'] = {path: sha(path) if Path(path).is_file() else None for path in before}
        report['inputs_unchanged'] = report['inputs_before'] == report['inputs_after']
        if not report['inputs_unchanged']:
            report['status'] = 'failed'
        save()
    if report['status'] != 'prepared':
        print(json.dumps({'status': report['status'], 'inputs_unchanged': report['inputs_unchanged'],
                          'error': report.get('error')}, ensure_ascii=False))
        raise SystemExit(1)
    prepared_path = out / 'prepared.json'
    prepared_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': 'prepared', 'inputs_unchanged': True, 'guest_started': False,
                      'prepared': str(prepared_path), 'prepared_sha256': sha(prepared_path)}, ensure_ascii=False))
    if not args.build_only:
        raise SystemExit(0 if run_prepared(prepared_path) else 1)


if __name__ == '__main__':
    main()
