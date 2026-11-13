#!/usr/bin/env python3
"""复用已验证的 Direct core 宿主，执行 Linux 的真实 BPF JIT 系统调用。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
from process_observer import OBSERVATION_MAX_BYTES, observe

ROOT = Path(__file__).resolve().parents[1]
PROGRAMS = {
    'bpf_jit_arithmetic': 14,
    'bpf_jit_helper': 41,
    'bpf_jit_array_lookup': 100,
    'bpf_jit_atomic64_fetch': 100,
    'bpf_jit_atomic64_cmpxchg': 105,
    'bpf_jit_atomic32_xchg': 7,
    'bpf_jit_atomic32_cmpxchg': 19,
    'bpf_jit_subprogram': 42,
    'bpf_jit_ringbuf_reserve_submit': 0,
}


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True, help='已通过且输入保持的单 CPU BPF JIT core 目录')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    baseline, out = args.baseline.resolve(), args.output.resolve()
    report_path = baseline / 'summary.json'
    report = json.loads(report_path.read_text())
    config = report['configuration']
    if (config['execution'] != 'direct' or config['mapping'] != 'pte' or config['cpus'] != 1
            or config['workload'] != 'core' or config['guest_layout'] != 'sparse'
            or not config['data_tbi'] or config['code_memory'] != 'dual'
            or config['direct']['memory'] != 'flat' or config.get('bpf_jit') is not True
            or config['guest_page_bytes'] not in (4096, 16384)
            or {r['entry'] for r in report['runs']} != {'cli', 'gcd'}
            or not all(r['returncode'] == 0 and r['validated'] for r in report['runs'])):
        raise ValueError('需要已通过的单 CPU Direct PTE/flat/sparse/TBI/dual BPF JIT core')
    compile_command = next(c.copy() for c in report['commands']
                           if '-c' in c and c[c.index('-c') + 1].endswith('/translation/linux-guest.c'))
    baseline_root = Path(compile_command[compile_command.index('-c') + 1]).parents[3]
    stale = [p for p, h in report['sha256'].items() if digest(baseline_root / p) != h]
    if stale:
        raise ValueError('基准输入发生变化: ' + ', '.join(stale))
    hosts = {}
    for entry in ('cli', 'gcd'):
        path = baseline / entry
        builds = [c for c in report['commands'] if '-o' in c
                  and Path(c[c.index('-o') + 1]).resolve() == path.resolve()]
        if len(builds) != 1:
            raise ValueError('基准宿主缺少唯一编译输出: ' + entry)
        output = Path(builds[0][builds[0].index('-o') + 1]).resolve(strict=True)
        manifest_path = str(output.relative_to(baseline_root))
        expected = report['sha256'].get(manifest_path)
        if not expected or not output.samefile(path) or digest(path) != expected:
            raise ValueError('实际宿主与编译输出或来源摘要不一致: ' + entry)
        hosts[entry] = {'path': str(path), 'compile_output': str(output),
                        'manifest_path': manifest_path, 'sha256': expected}
    kernel_image = baseline_root / next(p for p in report['sha256'] if p.endswith('/lkl-image.bin'))
    source = ROOT / 'experiments/native-linux/userspace/bpf-jit-test.c'
    uapi = baseline_root / 'work/native-linux-nce/processes/linux-uapi/include'
    out.mkdir(parents=True, exist_ok=False)
    commands = []
    runs = []
    state = {'scope': 'macOS Linux BPF syscalls and native ARM64 JIT execution; original loader and iPhone unverified',
             'baseline': str(baseline), 'base_report_sha256': digest(report_path),
             'base_inputs': report['sha256'], 'configuration': config,
             'host_inputs': hosts, 'commands': commands, 'runs': runs, 'status': 'running'}
    source_inputs = {str(p): digest(p) for p in
                     (source, Path(__file__), Path(observe.__code__.co_filename).resolve(), report_path)}
    source_inputs.update({host['path']: host['sha256'] for host in hosts.values()})
    state['sha256'] = source_inputs
    summary = out / 'summary.json'

    def save():
        summary.write_text(json.dumps(state, ensure_ascii=False, indent=2) + '\n')

    save()
    try:
        compile_command[compile_command.index('-c') + 1] = str(source)
        compile_command[compile_command.index('-o') + 1] = str(out / 'guest.o')
        compile_command += ['-idirafter', str(uapi), '-MD', '-MF', str(out / 'guest.d')]
        link = next(c.copy() for c in report['commands'] if c[-1] == str(baseline / 'guest.elf'))
        link = [str(out / 'guest.o') if x == str(baseline / 'guest.o') else x for x in link
                if not (x.startswith(str(baseline) + '/') and x.endswith('.o') and x != str(baseline / 'guest.o'))]
        link[-1] = str(out / 'guest.elf')
        state['toolchain'] = {name: {'path': command[0], 'resolved': str(Path(command[0]).resolve(strict=True))}
                              for name, command in (('clang', compile_command), ('lld', link))}
        state['link_inputs'] = [x for x in link[1:] if Path(x).is_file()]
        source_inputs.update({str(p): digest(p) for p in
                             (*state['link_inputs'], *(tool['path'] for tool in state['toolchain'].values()))})
        with (out / 'build.log').open('w') as build_log:
            commands.append(compile_command)
            save()
            subprocess.run(compile_command, check=True, stdout=build_log, stderr=subprocess.STDOUT, timeout=180)
            dependencies = shlex.split((out / 'guest.d').read_text().replace('\\\n', ' ').partition(':')[2])
            if not dependencies or str(source) not in dependencies:
                raise ValueError('缺少客体的实际编译依赖')
            state['compiler_dependencies'] = dependencies
            state['link_inputs'] = [x for x in link[1:] if Path(x).is_file()]
            for path in (*dependencies, *state['link_inputs'], out / 'guest.d'):
                source_inputs.setdefault(str(path), digest(path))
            commands.append(link)
            save()
            subprocess.run(link, check=True, stdout=build_log, stderr=subprocess.STDOUT, timeout=180)
        runtime_names = ('sigreturn.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so', 'dash.elf', 'bionic-test.elf')
        runtime_names += tuple(p.name for p in sorted(baseline.glob('exec-aslr-*.elf')))
        for name in runtime_names:
            (out / name).symlink_to(baseline / name)
        (out / 'late.elf').symlink_to(out / 'guest.elf')
        source_inputs[str(out / 'guest.elf')] = digest(out / 'guest.elf')
        environment = {k: v for k, v in os.environ.items() if not k.startswith(('NCE_', 'ANGLE_'))}
        environment.update(NCE_TRANSLATION_FILES=str(out), NCE_TRANSLATION_GUEST_LAYOUT='sparse',
                           NCE_TRANSLATION_DATA_TBI='1', NCE_TRANSLATION_ONLINE_CPUS='1',
                           NCE_TRANSLATION_BATCH=str(config['instruction_batch']), NCE_TRANSLATION_PARALLEL_PROFILE='0',
                           NCE_TRANSLATION_UACCESS_STOP=config['uaccess_stop'], NCE_TRANSLATION_PREEMPTION=config['preemption'],
                           NCE_TRANSLATION_MEMORY_MS=str(config['memory_interval_ms']), NCE_TRANSLATION_WORKLOAD='core',
                           NCE_TRANSLATION_CACHE=config['cache_invalidation'], NCE_TRANSLATION_EXEC_CACHE=config['exec_cache'],
                           NCE_TRANSLATION_TLB=config['tlb_invalidation'], NCE_TRANSLATION_LINUX_FASTPATH='0',
                           NCE_TRANSLATION_CACHE_MIB=str(config['translation_cache_bytes'] // (1024 * 1024)))
        for name, value in config['direct'].items():
            environment['NCE_DIRECT_' + name.upper()] = str(int(value)) if isinstance(value, bool) else str(value)
        state['environment'] = {k: v for k, v in environment.items() if k.startswith('NCE_')}
        for entry in ('cli', 'gcd'):
            if digest(hosts[entry]['path']) != hosts[entry]['sha256']:
                raise ValueError('实际宿主在启动前发生变化: ' + entry)
            command = [hosts[entry]['path'], str(kernel_image)]
            commands.append(command)
            stdout_path, stderr_path = out / (entry + '.jsonl'), out / (entry + '.log')
            started = time.monotonic()
            with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
                result = observe(command, environment, stdout, stderr, 180)
            result['wall_seconds'] = time.monotonic() - started
            rows, parse_errors = [], []
            with stdout_path.open('rb') as stdout, stderr_path.open('rb') as stderr:
                stdout_data = stdout.read(OBSERVATION_MAX_BYTES + 1)
                stderr_data = stderr.read(OBSERVATION_MAX_BYTES + 1)
            result['output_limit_exceeded'] |= max(len(stdout_data), len(stderr_data)) > OBSERVATION_MAX_BYTES
            kernel_pgtables_fault = b'BUG: non-zero pgtables_bytes on freeing mm:' in stderr_data
            for index, line in enumerate(stdout_data.decode(errors='replace').splitlines(), 1):
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError:
                    parse_errors.append(index)
            observed = {name: [r for r in rows if r.get('test') == name] for name in PROGRAMS}
            contexts = [r for r in rows if r.get('test') == 'translated_context_cleanup']
            validated = (result['returncode'] == result['actual_exit'] == 0 and not result['timed_out']
                         and not result['errors'] and not result['cleanup_signals'] and result['process_group_empty']
                         and not result['output_limit_exceeded'] and not kernel_pgtables_fault
                         and not parse_errors and all(len(observed[name]) == 1 and observed[name][0].get('passed') is True
                         and observed[name][0].get('retval') == expected and observed[name][0].get('jited_prog_len', 0) > 0
                         and observed[name][0].get('xlated_prog_len', 0) > 0 for name, expected in PROGRAMS.items())
                         and [r for r in rows if r.get('test') == 'bpf_jit_sysctls'] ==
                         [{'test': 'bpf_jit_sysctls', 'passed': True, 'bpf_jit_enable': 1, 'bpf_jit_kallsyms': 1}]
                         and [r for r in rows if r.get('test') == 'translated_bpf_code_pool_cleanup'] ==
                         [{'test': 'translated_bpf_code_pool_cleanup', 'passed': True}]
                         and [r for r in rows if r.get('test') == 'bpf_jit_guest_cleanup'] ==
                         [{'test': 'bpf_jit_guest_cleanup', 'passed': True}]
                         and [r for r in rows if r.get('test') == 'bpf_jit_ringbuf_record'] ==
                         [{'test': 'bpf_jit_ringbuf_record', 'passed': True, 'page_bytes': config['guest_page_bytes'],
                           'bytes': 4 * config['guest_page_bytes'], 'record_bytes': 16}]
                         and len(contexts) == 1 and contexts[0].get('allocated') == contexts[0].get('freed') == 1
                         and [r for r in rows if 'result' in r] == [{'result': 'passed'}])
            runs.append({'entry': entry, **result, 'validated': validated, 'parse_error_lines': parse_errors,
                         'kernel_pgtables_fault': kernel_pgtables_fault,
                         'results': rows, 'stdout_sha256': digest(stdout_path), 'stderr_sha256': digest(stderr_path)})
            save()
            print(json.dumps({'entry': entry, **result, 'validated': validated}), flush=True)
            if not validated:
                break
        state['inputs_unchanged'] = (all(digest(baseline_root / p) == h for p, h in report['sha256'].items())
                                     and all(digest(p) == h for p, h in source_inputs.items()))
        state['status'] = ('passed' if state['inputs_unchanged'] and len(runs) == 2
                           and all(r['validated'] for r in runs) else 'failed')
    except BaseException as error:
        state.update(status='failed', error=str(error), error_type=type(error).__name__)
        raise
    finally:
        save()
    if state['status'] != 'passed':
        raise SystemExit('BPF JIT 功能检查失败，保留本轮输出')


if __name__ == '__main__':
    main()
