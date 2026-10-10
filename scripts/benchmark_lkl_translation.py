#!/usr/bin/env python3
"""交替比较窗口表与直接 PTE 翻译，使用相同客体、抢占和内存采样设置。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from probe_lkl_translation import ROOT, WORK, validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeat', type=int, default=4)
    args = parser.parse_args()
    if args.repeat < 2 or args.repeat % 2:
        parser.error('repeat 必须为至少 2 的偶数，以平衡执行顺序')
    out = WORK / 'lkl-translation-matrix'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'summary.json').unlink(missing_ok=True)
    variants = {}
    for mapping in ('pte', 'window'):
        folder = WORK / f'lkl-translation-{mapping}'
        command = [sys.executable, ROOT / 'scripts/probe_lkl_translation.py', '--mapping', mapping,
                   '--preemption', 'timer', '--output', folder]
        with (out / f'prepare-{mapping}.log').open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
        variants[mapping] = json.loads((folder / 'summary.json').read_text())
    hashes = {}
    for report in variants.values():
        for path, value in report['sha256'].items():
            if path in hashes and hashes[path] != value:
                raise RuntimeError('比较变体使用的共同输入不一致: ' + path)
            hashes[path] = value
    hashes[str(Path(__file__).resolve().relative_to(ROOT))] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    # Equality includes the executable, stripped executable and dynamic libraries.
    guests = ('guest.elf', 'late.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so', 'dash.elf', 'bionic-test.elf')
    for name in guests:
        if (WORK / 'lkl-translation-pte' / name).read_bytes() != (WORK / 'lkl-translation-window' / name).read_bytes():
            raise RuntimeError('客体文件不一致: ' + name)

    def check_inputs():
        for path, expected in hashes.items():
            if hashlib.sha256((ROOT / path).read_bytes()).hexdigest() != expected:
                raise RuntimeError('测量输入已变化: ' + path)

    check_inputs()
    runs = []
    for iteration in range(args.repeat):
        for entry in ('cli', 'gcd'):
            for mapping in (('pte', 'window') if iteration % 2 == 0 else ('window', 'pte')):
                folder = WORK / f'lkl-translation-{mapping}'
                kernel = WORK / ('lkl-translation-kernel' if mapping == 'pte' else 'processes') / 'lkl-image.bin'
                env = os.environ.copy()
                env.update(NCE_TRANSLATION_FILES=str(folder), NCE_TRANSLATION_BATCH='50000',
                           NCE_TRANSLATION_PREEMPTION='timer', NCE_TRANSLATION_MEMORY_MS='100')
                stem = f'{iteration}-{entry}-{mapping}'
                with (out / f'{stem}.jsonl').open('w') as stdout, (out / f'{stem}.log').open('w') as stderr:
                    process = subprocess.run([folder / entry, kernel], env=env, stdout=stdout, stderr=stderr, timeout=120)
                rows = [json.loads(line) for line in (out / f'{stem}.jsonl').read_text().splitlines() if line.startswith('{')]
                passed = process.returncode == 0 and validate(rows, 100, mapping)
                runs.append({'iteration': iteration, 'entry': entry, 'mapping': mapping, 'results': rows,
                             'returncode': process.returncode, 'validated': passed,
                             'stdout_sha256': hashlib.sha256((out / f'{stem}.jsonl').read_bytes()).hexdigest(),
                             'stderr_sha256': hashlib.sha256((out / f'{stem}.log').read_bytes()).hexdigest()})
                print(stem + (': passed' if passed else ': failed'), flush=True)
                if not passed:
                    raise RuntimeError('页表对照失败: ' + stem)
    check_inputs()
    report = {'scope': 'Matched translated guest and preamble; balanced mapping order; 128 round trips per case',
              'platform': variants['pte']['platform'], 'cpu': variants['pte']['cpu'],
              'system_build': variants['pte']['system_build'],
              'configurations': {key: value['configuration'] for key, value in variants.items()},
              'sha256': hashes, 'runs': runs}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
