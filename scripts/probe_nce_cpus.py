#!/usr/bin/env python3
"""验证每 CPU 返回入口的并发覆盖、抢占目标和退出生命周期。"""
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
OUT = ROOT / 'work/native-linux-nce/cpu-probe'

TARGET_CHECK = r'''
#include <assert.h>
#include <pthread.h>
#include <signal.h>
static pthread_t delivered[4];
static unsigned count;
static int deliver(pthread_t thread, int signal) {
    assert(signal == SIGUSR1 && count < 4);
    delivered[count++] = thread;
    return 0;
}
#define pthread_kill deliver
#include "nce-cpu.c"
int main(void) {
    pthread_t first = (pthread_t)1, second = (pthread_t)2;
    nce_cpu_target(0, first);
    nce_cpu_target(1, second);
    nce_cpu_interrupt(SIGUSR1);
    assert(count == 2 && delivered[0] == first && delivered[1] == second);
    count = 0;
    nce_cpu_target(0, NULL);
    nce_cpu_target(1, first);
    nce_cpu_forget(second);
    nce_cpu_interrupt(SIGUSR1);
    assert(count == 1 && delivered[0] == first);
    count = 0;
    nce_cpu_forget(first);
    nce_cpu_interrupt(SIGUSR1);
    assert(count == 0);
}
'''


def run(command, **kwargs):
    return subprocess.run([str(value) for value in command], check=True, timeout=60, **kwargs)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    names = ['cpu-probe.c', 'nce-cpu.c', 'nce-cpu.h', 'nce-thread.c', 'nce-thread.h',
             'nce.c', 'nce.h', 'entry.S', 'guest-registers.h']
    inputs = [EXP / name for name in names] + [Path(__file__).resolve()]
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    hashes = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    flags = ['xcrun', 'clang', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-isysroot', sdk, '-I', EXP]
    core = [EXP / name for name in ['nce-thread.c', 'nce.c', 'entry.S']]
    run([*flags, EXP / 'cpu-probe.c', EXP / 'nce-cpu.c', *core, '-o', OUT / 'probe'])
    (OUT / 'target-check.c').write_text(TARGET_CHECK)
    run([*flags, OUT / 'target-check.c', *core, '-o', OUT / 'target-check'])
    run([OUT / 'target-check'])
    rows = []
    for entry in ['cli', 'gcd']:
        with (OUT / (entry + '.jsonl')).open('w') as output, (OUT / (entry + '.log')).open('w') as error:
            run([OUT / 'probe', *(['gcd'] if entry == 'gcd' else [])], stdout=output, stderr=error)
        records = [json.loads(line) for line in (OUT / (entry + '.jsonl')).read_text().splitlines()]
        assert records[-1] == {'result': 'passed'}
        assert len([r for r in records if r.get('passed')]) == 5
        rows.append({'entry': entry, 'records': records})
    ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', '-target', 'arm64-apple-ios17.0',
         '-isysroot', ios, '-I', EXP, '-c', EXP / 'nce-cpu.c', '-o', OUT / 'ios-nce-cpu.o'])
    assert hashes == {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    report = {'scope': 'two host threads with exclusive CPU slots; Linux MMU concurrency pending',
              'host': {'platform': platform.platform(),
                       'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()},
              'ios': 'compiled; device execution pending', 'inputs': hashes,
              'target_lifecycle': 'passed', 'runs': rows,
              'raw': {name: digest(OUT / name) for name in ['cli.jsonl', 'cli.log', 'gcd.jsonl', 'gcd.log']}}
    (OUT / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(rows, indent=2))


if __name__ == '__main__':
    main()
