#!/usr/bin/env python3
"""用真实子进程检查组件 stdout、退出、exec 失败和超时回收。"""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / 'work/component-supervisor-test'
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'supervisor-test'
    command = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
               str(ROOT / 'tests/native-linux/component-supervisor.c'), '-o', str(binary)]
    subprocess.run(command, check=True, capture_output=True, timeout=60)
    cases = [
        ('output', ['/bin/sh', '-c', 'printf "Us"; printf "age:\\n{\\"child\\":true}\\n"; exit 0'],
         'Usage:\n{"child":true}\n', dict(exit_code=0, signal=0, exec_errno=0, usage=1, timed_out=0)),
        ('failure', ['/bin/sh', '-c', 'printf "before failure\\n"; exit 7'],
         'before failure\n', dict(exit_code=7, signal=0, exec_errno=0, usage=0, timed_out=0)),
        ('signal', ['/bin/sh', '-c', 'printf "before signal\\n"; kill -TERM $$'],
         'before signal\n', dict(exit_code=-1, signal=15, exec_errno=0, usage=0, timed_out=0)),
        ('exec_failure', ['/no-such-stock-component'], '',
         dict(exit_code=127, signal=0, exec_errno=2, usage=0, timed_out=0)),
        ('timeout_open_pipe', ['/bin/sh', '-c', 'printf "before timeout\\n"; exec /bin/sleep 3'],
         'before timeout\n', dict(exit_code=-1, signal=9, exec_errno=0, usage=0, timed_out=1)),
        ('timeout_after_eof', ['/bin/sh', '-c', 'printf "before EOF\\n"; exec 1>&-; exec /bin/sleep 3'],
         'before EOF\n', dict(exit_code=-1, signal=9, exec_errno=0, usage=0, timed_out=1)),
    ]
    results = []
    for name, arguments, prefix, expected in cases:
        run = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=5)
        assert run.returncode == 0, (name, run.stderr)
        lines = run.stdout.splitlines(keepends=True)
        assert ''.join(lines[:-1]) == prefix, (name, run.stdout)
        assert json.loads(lines[-1]) == expected, (name, run.stdout)
        assert ('component timeout:' in run.stderr) == bool(expected['timed_out']), (name, run.stderr)
        results.append({'case': name, 'passed': True, 'result': expected})
    (output / 'results.json').write_text(json.dumps({'compile': command, 'checks': results}, indent=2) + '\n')
    print(f'component supervisor: {len(results)} real subprocess checks passed')


if __name__ == '__main__':
    main()
