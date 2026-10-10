#!/usr/bin/env python3
"""交替比较相同 Bionic/ART 负载的翻译缓存设置，保留阶段内存采样。"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from probe_lkl_translation import ROOT, WORK, sha, validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeat', type=int, default=2)
    parser.add_argument('--comparison', choices=('cache', 'tlb'), default='cache')
    args = parser.parse_args()
    if args.repeat < 2 or args.repeat % 2:
        parser.error('repeat 必须为至少 2 的偶数，以平衡执行顺序')
    out = WORK / ('android-translation-matrix' if args.comparison == 'cache' else 'android-translation-tlb-matrix')
    folder = out / 'runtime'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'summary.json').unlink(missing_ok=True)
    command = [sys.executable, ROOT / 'scripts/probe_lkl_translation.py', '--mapping', 'pte',
               '--preemption', 'timer', '--workload', 'android', '--output', folder]
    if args.comparison == 'tlb':
        command += ['--cache-mib', '128', '--linux-fastpath', '1']
    with (out / 'prepare.log').open('w') as log:
        subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    baseline = json.loads((folder / 'summary.json').read_text())
    hashes = {**baseline['sha256'], str(Path(__file__).resolve().relative_to(ROOT)): sha(Path(__file__))}
    common = {key: value for key, value in baseline['configuration'].items()
              if key not in ('cache_invalidation', 'linux_fastpath', 'translation_cache_bytes', 'tlb_invalidation')}
    configurations = {
        'full-16': {'cache_invalidation': 'full', 'linux_fastpath': 0, 'cache_mib': 16},
        'pages-16': {'cache_invalidation': 'pages', 'linux_fastpath': 0, 'cache_mib': 16},
        'linux-16': {'cache_invalidation': 'pages', 'linux_fastpath': 1, 'cache_mib': 16},
        'linux-64': {'cache_invalidation': 'pages', 'linux_fastpath': 1, 'cache_mib': 64},
    }
    if args.comparison == 'tlb':
        configurations = {name: {'cache_invalidation': 'pages', 'linux_fastpath': 1, 'cache_mib': 128,
                                 'tlb_invalidation': name} for name in ('full', 'pages')}
    else:
        for config in configurations.values():
            config['tlb_invalidation'] = 'full'

    def check_inputs():
        for path, expected in hashes.items():
            if sha(ROOT / path) != expected:
                raise RuntimeError('测量输入已变化: ' + path)

    check_inputs()
    runs = []
    for iteration in range(args.repeat):
        for entry in ('cli', 'gcd'):
            order = list(configurations)
            if (iteration + (entry == 'gcd')) % 2:
                order.reverse()
            for name in order:
                config = configurations[name]
                env = dict(os.environ, NCE_TRANSLATION_FILES=str(folder), NCE_TRANSLATION_WORKLOAD='android',
                           NCE_TRANSLATION_SYSTEM=str(WORK / 'processes/system.img'),
                           NCE_TRANSLATION_BATCH='50000', NCE_TRANSLATION_PREEMPTION='timer',
                           NCE_TRANSLATION_MEMORY_MS='100', NCE_TRANSLATION_CACHE=config['cache_invalidation'],
                           NCE_TRANSLATION_EXEC_CACHE='full',
                           NCE_TRANSLATION_TLB=config['tlb_invalidation'],
                           NCE_TRANSLATION_LINUX_FASTPATH=str(config['linux_fastpath']),
                           NCE_TRANSLATION_CACHE_MIB=str(config['cache_mib']))
                stem = f'{iteration}-{entry}-{name}'
                with (out / f'{stem}.jsonl').open('w') as stdout, (out / f'{stem}.log').open('w') as stderr:
                    try:
                        process = subprocess.run([folder / entry, WORK / 'lkl-translation-kernel/lkl-image.bin'],
                                                 env=env, stdout=stdout, stderr=stderr, timeout=120)
                        returncode = process.returncode
                    except subprocess.TimeoutExpired:
                        returncode = 'timeout'
                rows = [json.loads(line) for line in (out / f'{stem}.jsonl').read_text().splitlines() if line.startswith('{')]
                passed = returncode == 0 and validate(rows, 100, 'pte', 'android', config['tlb_invalidation'])
                runs.append({'iteration': iteration, 'entry': entry, 'variant': name, 'results': rows,
                             'returncode': returncode, 'validated': passed,
                             'stdout_sha256': sha(out / f'{stem}.jsonl'), 'stderr_sha256': sha(out / f'{stem}.log')})
                report = {'scope': 'Same executable, system image and DEX; balanced configuration order',
                          'platform': baseline['platform'], 'cpu': baseline['cpu'], 'system_build': baseline['system_build'],
                          'common_configuration': common, 'variants': configurations,
                          'sha256': hashes, 'runs': runs}
                (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
                print(stem + (': passed' if passed else ': failed'), flush=True)
                if not passed:
                    raise RuntimeError('Android 翻译对照失败: ' + stem)
    check_inputs()
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
