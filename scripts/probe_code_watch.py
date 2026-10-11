#!/usr/bin/env python3
"""验证物理代码页的写入跟踪、异常转发和权限恢复。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=WORK / 'code-watch-tests')
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(WORK) or platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('需要 Apple Silicon Mac，产物放在 work/native-linux-nce 下')
    out.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'experiments/native-linux/translation'
    inputs = [source / name for name in ('code-watch.c', 'code-watch.h', 'code-watch-test.c')] + [Path(__file__)]
    hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    command = ['xcrun', 'clang', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               str(source / 'code-watch.c'), str(source / 'code-watch-test.c'), '-o', str(out / 'probe')]
    subprocess.run(command, check=True, timeout=120)
    expected = {'configuration_and_bounds', 'both_aliases_and_reprotection', 'unrelated_read_and_write_forwarding',
                'concurrent_fault_retry', 'backing_replacement', 'protection_failure_and_retry',
                'partial_protection_rollback', 'additional_alias_uses_content_checks',
                'kernel_copyout_after_invalidation', 'shutdown_restores_permissions_and_handlers'}
    report = {'platform': platform.platform(), 'command': command, 'sha256': hashes, 'runs': []}
    hashes[str((out / 'probe').relative_to(ROOT))] = hashlib.sha256((out / 'probe').read_bytes()).hexdigest()
    for entry in ('cli', 'gcd'):
        with (out / f'{entry}.jsonl').open('w') as stdout, (out / f'{entry}.log').open('w') as stderr:
            result = subprocess.run([out / 'probe', entry], stdout=stdout, stderr=stderr, timeout=60)
        rows = [json.loads(line) for line in (out / f'{entry}.jsonl').read_text().splitlines()]
        passed = result.returncode == 0 and rows and rows[-1].get('result') == 'passed' and all(
            row.get('passed', True) for row in rows) and {
                'code_watch_' + name for name in expected} <= {row.get('test') for row in rows}
        report['runs'].append({'entry': entry, 'returncode': result.returncode, 'validated': bool(passed), 'results': rows})
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
        if not passed:
            raise SystemExit(f'检查失败：{out / entry}')
        print(entry + ': passed', flush=True)
    for name, digest in hashes.items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('检查期间输入变化：' + name)


if __name__ == '__main__':
    main()
