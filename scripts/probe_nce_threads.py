#!/usr/bin/env python3
"""验证共享 NCE 代码上的线程状态、TLS、寄存器及定向信号。"""
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
OUT = ROOT / 'work/native-linux-nce/thread-probe'


def run(command, **kwargs):
    return subprocess.run([str(value) for value in command], check=True, timeout=60, **kwargs)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    names = ['thread-guest.S', 'thread-probe.c', 'nce-thread.c', 'nce-thread.h',
             'nce.c', 'nce.h', 'entry.S', 'guest-registers.h']
    inputs = [EXP / name for name in names] + [Path(__file__).resolve()]
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    hashes = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    run(['xcrun', 'clang', '--target=aarch64-none-elf', '-c', EXP / 'thread-guest.S', '-o', OUT / 'guest.o'])
    llvm = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    run([llvm / 'llvm-objcopy', '--only-section=.text', '-O', 'binary', OUT / 'guest.o', OUT / 'guest.bin'])
    payload = (OUT / 'guest.bin').read_bytes()
    (OUT / 'thread-image.h').write_text('static const unsigned char thread_image[] = {' +
                                      ','.join(map(str, payload)) + '};\n')
    sources = [EXP / name for name in ['thread-probe.c', 'nce-thread.c', 'nce.c', 'entry.S']]
    run(['xcrun', 'clang', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-isysroot', sdk,
         '-I', OUT, '-I', EXP, *sources, '-o', OUT / 'probe'])
    rows = []
    for entry in ['cli', 'gcd']:
        with (OUT / (entry + '.jsonl')).open('w') as output, (OUT / (entry + '.log')).open('w') as error:
            run([OUT / 'probe', *(['gcd'] if entry == 'gcd' else [])], stdout=output, stderr=error)
        records = [json.loads(line) for line in (OUT / (entry + '.jsonl')).read_text().splitlines()]
        assert records[-1] == {'result': 'passed'}
        assert len([r for r in records if r.get('passed')]) == 4
        rows.append({'entry': entry, 'records': records})
    ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    for name in ['nce-thread.c', 'nce.c', 'entry.S']:
        run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', '-target', 'arm64-apple-ios17.0',
             '-isysroot', ios, '-I', EXP, '-c', EXP / name, '-o', OUT / ('ios-' + name + '.o')])
    assert hashes == {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    report = {'scope': 'shared native guest code on two pthreads; no Linux scheduling or mapping changes',
              'host': {'platform': platform.platform(),
                       'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip()},
              'ios': 'compiled; device execution pending', 'inputs': hashes, 'runs': rows,
              'raw': {name: digest(OUT / name) for name in ['cli.jsonl', 'cli.log', 'gcd.jsonl', 'gcd.log']}}
    (OUT / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(rows, indent=2))


if __name__ == '__main__':
    main()
