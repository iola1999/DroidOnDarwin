#!/usr/bin/env python3
"""验证并发执行分段的线程归属、累计结果与关闭行为。"""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'work/native-linux-nce/execution-profile-tests'
OUT.mkdir(parents=True, exist_ok=True)
subprocess.run(['xcrun', 'clang', '-std=c11', '-D_DARWIN_C_SOURCE', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'experiments/native-linux'), str(ROOT / 'tests/performance/execution-profile.c'),
                str(ROOT / 'experiments/native-linux/translation/interaction-profile.c'), '-o', str(OUT / 'probe')],
               check=True, timeout=60)
subprocess.run([str(OUT / 'probe'), str(OUT)], check=True, timeout=15)
result = json.loads((OUT / 'execution.json').read_text())
assert result['overflow'] == result['clock_errors'] == 0
assert result['fallback_overflow'] == 0
assert len(result['fallbacks']) == 4
assert len(result['threads']) == 2
assert len({thread['host_tid'] for thread in result['threads']}) == 2
for thread in result['threads']:
    assert thread['stages'][1]['calls'] == 100
    assert thread['stages'][1]['wall_ns'] >= thread['stages'][1]['cpu_ns'] > 0
    assert thread['stages'][0]['calls'] == 0
    assert thread['stages'][2]['calls'] == 100
    intervals = [row for row in result['fallbacks'] if row['host_tid'] == thread['host_tid']]
    assert {row['instruction'] for row in intervals} == {0x7ea1d401, 0x885ffe69}
    assert all(row['calls'] == 50 for row in intervals)
    for key in ('calls', 'cpu_ns', 'wall_ns'):
        assert sum(row[key] for row in intervals) == thread['stages'][2][key]
print('execution profile: concurrency, thread IDs, CPU/wall time and disabled window passed')
