#!/usr/bin/env python3
"""检查宿主磁盘失败后的关闭、锁释放和实际宿主代码的 iOS 编译。"""
import argparse
from collections import Counter
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-ops', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    header = args.host_ops.resolve(strict=True)
    if header.name != 'lkl-host-ops.h':
        parser.error('--host-ops 需要生成内核使用的 lkl-host-ops.h')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    host, fixture = EXP / 'lkl-host.c', EXP / 'host-context-test.c'
    inputs = [host, fixture, EXP / 'layout.h', header, Path(__file__).resolve()]
    report = {'test': 'host_disk_close_failures', 'passed': False, 'host': platform.platform(),
              'sha256': {str(path): sha(path) for path in inputs}, 'commands': []}
    mac_sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    ios_sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    common = ['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(EXP), '-I', str(header.parent)]
    commands = [
        common + ['-isysroot', mac_sdk, '-Dfsync=nce_host_test_fsync', '-Dclose=nce_host_test_close',
                  '-c', str(host), '-o', str(out / 'host-failure.o')],
        common + ['-isysroot', mac_sdk, '-DNCE_HOST_DISK_FAILURE_TEST', str(fixture),
                  str(out / 'host-failure.o'), '-o', str(out / 'host-context-test')],
        common + ['-DNCE_PROCESS', '-target', 'arm64-apple-ios26.0', '-isysroot', ios_sdk,
                  '-c', str(host), '-o', str(out / 'ios-lkl-host.o')],
    ]
    try:
        with (out / 'build.log').open('w') as log:
            for command in commands:
                report['commands'].append(command)
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        command = [str(out / 'host-context-test')]
        report['commands'].append(command)
        with (out / 'stdout.jsonl').open('w') as stdout, (out / 'stderr.log').open('w') as stderr:
            process = subprocess.run(command, stdout=stdout, stderr=stderr,
                                     env={**os.environ, 'TMPDIR': str(out)}, timeout=60)
        report['returncode'] = process.returncode
        rows = [json.loads(line) for line in (out / 'stdout.jsonl').read_text().splitlines()]
        tests = [row for row in rows if 'test' in row]
        expected = Counter([('host_disk_bounds_readonly_and_locking', ''),
                            ('host_context_preserves_thread_masks', '')] +
                           [('host_disk_failure_closes_all_slots', name)
                            for name in ('sync_error', 'sync_and_close_error', 'close_error')])
        actual = Counter((row['test'], row.get('case', '')) for row in tests)
        closes = [row for row in rows if row.get('host_disk_close') is True]
        expected_closes = []
        for sync_result, close_result in ((0, 0), (-errno.EIO, 0),
                                          (-errno.EIO, -errno.EINTR), (0, -errno.EINTR)):
            expected_closes += [{'host_disk_close': True, 'slot': slot, 'sync_attempted': slot == 0,
                                'sync_result': sync_result if slot == 0 else 0,
                                'close_result': close_result if slot == 0 else 0} for slot in range(4)]
        report['results'] = rows
        report['ios_compiled'] = True
        report['inputs_unchanged'] = all(sha(Path(path)) == digest for path, digest in report['sha256'].items())
        report['passed'] = (process.returncode == 0 and actual == expected and
                            all(row.get('passed') is True for row in tests) and closes == expected_closes and
                            report['inputs_unchanged'])
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    report['artifacts'] = {path.name: {'bytes': path.stat().st_size, 'sha256': sha(path)}
                           for path in out.iterdir() if path.is_file()}
    (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    if not report['passed']:
        (out / 'failure.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'report': str(out / 'result.json')}, ensure_ascii=False))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
