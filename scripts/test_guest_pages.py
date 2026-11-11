#!/usr/bin/env python3
"""调用运行器实际宿主别名检查，验证 4 KiB 请求的拒绝边界。"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'
EXP = ROOT / 'experiments/native-linux'


def main():
    out = WORK / 'guest-4k-alias-test'
    out.mkdir(parents=True, exist_ok=True)
    harness = ROOT / 'tests/native-linux/guest-pages.c'
    kernel = WORK / 'lkl-translation-kernel-4k'
    command = ['xcrun', 'clang', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-DNCE_GUEST_PAGE_SHIFT=12', '-Wl,-dead_strip', '-I', EXP, '-I', kernel,
               '-I', WORK / 'processes/host-include', '-I', WORK / 'unicorn-tb-stop-dual/prefix/include',
               harness, '-o', out / 'test']
    subprocess.run(command, check=True, timeout=60)
    target, source, host_page = 0x1000000000, 0x2000000000, 16384
    ram, reservation = 1 << 30, (1 << 30) + (64 << 20)
    cases = [
        ('initial-ram', 'map', target, source, ram, True),
        ('last-host-page', 'map', target + reservation - host_page, source + ram - host_page, host_page, True),
        ('revoke', 'revoke', target, 0, host_page, True),
        ('reclaim', 'reclaim', target + 65536, source + 65536, 65536, True),
        ('target-4k', 'map', target + 4096, source, host_page, False),
        ('source-4k', 'map', target, source + 4096, host_page, False),
        ('length-4k', 'map', target, source, 4096, False),
        ('first-vmalloc-request', 'map', target + ram + (8 << 20), source + 0x811000, 4096, False),
        ('empty', 'map', target, source, 0, False),
        ('target-before', 'map', target - host_page, source, host_page, False),
        ('target-end', 'map', target + reservation, source, host_page, False),
        ('target-cross-end', 'map', target + reservation - host_page, source, 2 * host_page, False),
        ('source-before', 'map', target, source - host_page, host_page, False),
        ('source-end', 'map', target, source + ram, host_page, False),
        ('source-cross-end', 'map', target, source + ram - host_page, 2 * host_page, False),
        ('target-overflow', 'map', (1 << 64) - host_page, source, 2 * host_page, False),
        ('length-overflow', 'map', target, source, (1 << 64) - host_page, False),
        ('revoke-4k', 'revoke', target, 0, 4096, False),
        ('reclaim-4k', 'reclaim', target + 4096, source + 4096, 65536, False),
    ]
    results = []
    for name, operation, address, backing, size, accepted in cases:
        process = subprocess.run([out / 'test', operation, str(address), str(backing), str(size)],
                                 text=True, capture_output=True, timeout=10)
        expected = {'host_alias_request': operation, 'target': address, 'source': backing,
                    'bytes': size, 'accepted': accepted}
        rows = [json.loads(line) for line in process.stdout.splitlines() if line.startswith('{')]
        passed = (process.returncode == (0 if accepted else 86) and rows == [expected] and
                  (not process.stderr if accepted else '4 KiB guest rejected host alias:' in process.stderr))
        results.append({'case': name, 'validated': passed, 'returncode': process.returncode,
                        'stdout': process.stdout, 'stderr': process.stderr})
    base = [sys.executable, '-B', str(ROOT / 'scripts/probe_lkl_translation.py'), '--guest-page-bytes', '4096',
            '--mapping', 'pte', '--execution', 'unicorn', '--guest-layout', 'low', '--build-only']
    invalid_options = [('--cpus', '2'), ('--mapping', 'window'), ('--execution', 'direct'),
                       ('--guest-layout', 'host'), ('--workload', 'android'), ('--workload', 'desktop')]
    for option in invalid_options:
        process = subprocess.run([*base, *option], text=True, capture_output=True, timeout=10)
        passed = process.returncode == 2 and '4 KiB 实验需要' in process.stderr
        results.append({'case': 'arguments-' + '-'.join(option), 'validated': passed,
                        'returncode': process.returncode, 'stdout': process.stdout, 'stderr': process.stderr})
    files = [Path(__file__).resolve(), harness, EXP / 'translation/linux-execution.c', EXP / 'layout.h',
             ROOT / 'scripts/probe_lkl_translation.py',
             EXP / 'translation/kernel-api.h', kernel / 'lkl-image.h', kernel / 'lkl-host-ops.h', out / 'test']
    report = {'scope': 'Actual host alias precondition; no mappings or Linux execution',
              'command': list(map(str, command)), 'runs': results,
              'sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    assert all(row['validated'] for row in results)
    print(f'host alias boundary checks: {len(results)} passed')


if __name__ == '__main__':
    main()
