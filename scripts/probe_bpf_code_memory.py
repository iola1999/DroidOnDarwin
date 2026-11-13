#!/usr/bin/env python3
"""检查 BPF JIT 所需的宿主执行池、调用 ABI 与生命周期。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(WORK) or platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('需要 Apple Silicon Mac，产物放在独立树的 work/native-linux-nce 下')
    out.mkdir(parents=True, exist_ok=False)
    exp = ROOT / 'experiments/native-linux'
    files = [exp / name for name in ('translation/bpf-code-memory.c', 'translation/bpf-code-memory.h',
                                     'bpf-code-memory-test.c', 'code-memory.c', 'code-memory.h', 'nce.h', 'guest-registers.h',
                                     'execute-check.c', 'execute-check.h')]
    files += [Path(__file__), ROOT / 'src/ios-jit/ios-jit.h']
    hashes = {str(p.relative_to(ROOT)): digest(p) for p in files}
    command = ['xcrun', 'clang', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               str(exp / 'translation/bpf-code-memory.c'), str(exp / 'code-memory.c'), str(exp / 'execute-check.c'),
               str(exp / 'bpf-code-memory-test.c'), '-o', str(out / 'probe')]
    subprocess.run(command, check=True, timeout=120)
    expected = {'bpf_code_allocation_and_exhaustion', 'bpf_code_callback_abi_bounds_and_rx_permission',
                'bpf_code_parallel_publication', 'bpf_code_reuse_and_release', 'bpf_code_stopped_kernel_release'}
    report = {'scope': 'macOS host execution pool for native Linux BPF JIT; BPF program execution follows separately',
              'platform': platform.platform(), 'pool_bytes': 32768, 'host_page_bytes': 16384,
              'commands': [command], 'sha256': hashes, 'runs': []}
    hashes[str((out / 'probe').relative_to(ROOT))] = digest(out / 'probe')
    for entry in ('cli', 'gcd'):
        run = [str(out / 'probe'), entry]
        report['commands'].append(run)
        with (out / (entry + '.jsonl')).open('w') as stdout, (out / (entry + '.log')).open('w') as stderr:
            try:
                result = subprocess.run(run, stdout=stdout, stderr=stderr, timeout=60)
                returncode, timed_out = result.returncode, False
            except subprocess.TimeoutExpired:
                returncode, timed_out = -1, True
        rows = [json.loads(line) for line in (out / (entry + '.jsonl')).read_text().splitlines() if line.startswith('{')]
        passed = (returncode == 0 and not timed_out and rows and rows[-1].get('result') == 'passed'
                  and all(row.get('passed', True) for row in rows)
                  and {row.get('test') for row in rows if 'test' in row} == expected)
        report['runs'].append({'entry': entry, 'returncode': returncode, 'timed_out': timed_out, 'validated': bool(passed),
                               'results': rows, 'stdout_sha256': digest(out / (entry + '.jsonl')),
                               'stderr_sha256': digest(out / (entry + '.log'))})
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
        print(entry, 'passed' if passed else 'failed', flush=True)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    report['ios_compile'] = {'sdk': sdk, 'scope': 'execution pool and existing preparation/check helpers; device execution not run', 'commands': []}
    with (out / 'ios-compile.log').open('w') as log:
        for name in ('translation/bpf-code-memory.c', 'code-memory.c', 'execute-check.c'):
            command = ['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios26.0', '-isysroot', sdk,
                       '-I', str(ROOT / 'src/ios-jit'), '-O2', '-Wall', '-Wextra', '-Werror', '-c', str(exp / name),
                       '-o', str(out / ('ios-' + Path(name).stem + '.o'))]
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
            report['ios_compile']['commands'].append({'command': command, 'returncode': result.returncode})
    report['ios_compile']['passed'] = all(c['returncode'] == 0 for c in report['ios_compile']['commands'])
    unchanged = all(digest(ROOT / p) == h for p, h in hashes.items())
    report['inputs_unchanged'] = unchanged
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    if not unchanged or not report['ios_compile']['passed'] or any(not r['validated'] for r in report['runs']):
        raise SystemExit('BPF 执行池检查失败，保留本轮输出')


if __name__ == '__main__':
    main()
