#!/usr/bin/env python3
"""交替比较相同 Bionic/ART 负载的执行器与缓存设置，保留阶段内存采样。"""
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
    parser.add_argument('--comparison', choices=('cache', 'tlb', 'execution', 'direct-mappings'), default='cache')
    args = parser.parse_args()
    if args.repeat < 2 or args.repeat % 2:
        parser.error('repeat 必须为至少 2 的偶数，以平衡执行顺序')
    suffix = '' if args.comparison == 'cache' else '-' + args.comparison
    out = WORK / f'android-translation{suffix}-matrix'
    engines = ('unicorn', 'direct') if args.comparison == 'execution' else ('direct',) if args.comparison == 'direct-mappings' else ('unicorn',)
    folders = {name: out / ('runtime-' + name if args.comparison == 'execution' else 'runtime') for name in engines}
    out.mkdir(parents=True, exist_ok=True)
    (out / 'summary.json').unlink(missing_ok=True)
    prepared = {}
    for execution, folder in folders.items():
        command = [sys.executable, ROOT / 'scripts/probe_lkl_translation.py', '--mapping', 'pte',
                   '--preemption', 'timer', '--workload', 'android', '--output', folder, '--execution', execution]
        if prepared:
            command += ['--reuse-engine-build']
        if args.comparison != 'cache':
            command += ['--cache-mib', '128', '--linux-fastpath', '1']
        with (out / f'prepare-{execution}.log').open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
        prepared[execution] = json.loads((folder / 'summary.json').read_text())
    baseline = next(iter(prepared.values()))
    hashes = {str(Path(__file__).resolve().relative_to(ROOT)): sha(Path(__file__))}
    for result in prepared.values():
        for name, digest in result['sha256'].items():
            if name in hashes and hashes[name] != digest:
                raise RuntimeError('执行器构建期间输入发生变化：' + name)
            hashes[name] = digest
    common = {key: value for key, value in baseline['configuration'].items()
              if key not in ('execution', 'direct', 'cache_invalidation', 'linux_fastpath', 'translation_cache_bytes', 'tlb_invalidation')}
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

    if args.comparison == 'execution':
        configurations = {name: {'execution': name, 'cache_invalidation': 'pages', 'linux_fastpath': 1,
                                 'cache_mib': 128, 'tlb_invalidation': 'full'} for name in folders}
    elif args.comparison == 'direct-mappings':
        configurations = {
            name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 1,
                   'cache_mib': 128, 'tlb_invalidation': 'full',
                   'direct': {'mapping_entries': maps, 'route_entries': routes, 'selective_writes': selective}}
            for name, maps, routes, selective in (
                ('full-256', 256, 256, False), ('selective-256', 256, 256, True),
                ('mapping-4096', 4096, 256, True), ('route-4096', 256, 4096, True),
                ('both-4096', 4096, 4096, True))}
    else:
        for config in configurations.values():
            config['execution'] = 'unicorn'
    for config in configurations.values():
        if config['execution'] == 'direct' and 'direct' not in config:
            config['direct'] = {'mapping_entries': 256, 'route_entries': 256, 'selective_writes': False}

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
                folder = folders[config['execution']]
                env = dict(os.environ, NCE_TRANSLATION_FILES=str(folder), NCE_TRANSLATION_WORKLOAD='android',
                           NCE_TRANSLATION_SYSTEM=str(WORK / 'processes/system.img'),
                           NCE_TRANSLATION_BATCH='50000', NCE_TRANSLATION_PREEMPTION='timer',
                           NCE_TRANSLATION_MEMORY_MS='100', NCE_TRANSLATION_CACHE=config['cache_invalidation'],
                           NCE_TRANSLATION_EXEC_CACHE='full',
                           NCE_TRANSLATION_TLB=config['tlb_invalidation'],
                           NCE_TRANSLATION_LINUX_FASTPATH=str(config['linux_fastpath']),
                           NCE_TRANSLATION_CACHE_MIB=str(config['cache_mib']))
                direct = config.get('direct')
                if direct:
                    env.update(NCE_DIRECT_MAPPING_ENTRIES=str(direct['mapping_entries']),
                               NCE_DIRECT_ROUTE_ENTRIES=str(direct['route_entries']),
                               NCE_DIRECT_SELECTIVE_WRITES=str(int(direct['selective_writes'])))
                stem = f'{iteration}-{entry}-{name}'
                with (out / f'{stem}.jsonl').open('w') as stdout, (out / f'{stem}.log').open('w') as stderr:
                    try:
                        process = subprocess.run([folder / entry, WORK / 'lkl-translation-kernel/lkl-image.bin'],
                                                 env=env, stdout=stdout, stderr=stderr, timeout=120)
                        returncode = process.returncode
                    except subprocess.TimeoutExpired:
                        returncode = 'timeout'
                rows = [json.loads(line) for line in (out / f'{stem}.jsonl').read_text().splitlines() if line.startswith('{')]
                passed = returncode == 0 and validate(rows, 100, 'pte', 'android', config['tlb_invalidation'], config['execution'], direct)
                runs.append({'iteration': iteration, 'entry': entry, 'variant': name, 'results': rows,
                             'returncode': returncode, 'validated': passed,
                             'stdout_sha256': sha(out / f'{stem}.jsonl'), 'stderr_sha256': sha(out / f'{stem}.log')})
                report = {'scope': 'Same guest/kernel inputs; separate host executables for execution comparison; balanced configuration order',
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
