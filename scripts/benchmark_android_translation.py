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
    parser.add_argument('--comparison', choices=('cache', 'tlb', 'execution', 'direct-mappings', 'direct-cache', 'direct-memory', 'direct-flat', 'direct-code-watch', 'direct-cross-page', 'direct-routes'), default='cache')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.repeat < 2 or args.repeat % 2:
        parser.error('repeat 必须为至少 2 的偶数，以平衡执行顺序')
    suffix = '' if args.comparison == 'cache' else '-' + args.comparison
    out = (args.output or WORK / f'android-translation{suffix}-matrix').resolve()
    if not out.is_relative_to(WORK.resolve()):
        parser.error('output 必须位于 work/native-linux-nce 下')
    engines = ('unicorn', 'direct') if args.comparison == 'execution' else ('direct',) if args.comparison.startswith('direct-') else ('unicorn',)
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
    elif args.comparison.startswith('direct-'):
        configurations = {
            name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 1,
                   'cache_mib': 128, 'tlb_invalidation': 'full',
                   'direct': {'mapping_entries': maps, 'route_entries': routes, 'selective_writes': selective, 'reuse_code_checks': False}}
            for name, maps, routes, selective in (
                ('full-256', 256, 256, False), ('selective-256', 256, 256, True),
                ('mapping-4096', 4096, 256, True), ('route-4096', 256, 4096, True),
                ('both-4096', 4096, 4096, True))}
        if args.comparison == 'direct-cache':
            for entries in (256, 4096):
                configurations[f'reuse-{entries}'] = {
                    'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 1,
                    'cache_mib': 128, 'tlb_invalidation': 'full',
                    'direct': {'mapping_entries': entries, 'route_entries': entries, 'selective_writes': True, 'reuse_code_checks': True}}
    else:
        for config in configurations.values():
            config['execution'] = 'unicorn'
    if args.comparison in ('direct-memory', 'direct-flat', 'direct-code-watch', 'direct-cross-page', 'direct-routes'):
        configurations = {name: configurations[name] for name in ('full-256', 'mapping-4096')}
        configurations['pte'] = {
            'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 1,
            'cache_mib': 128, 'tlb_invalidation': 'full',
            'direct': {'mapping_entries': 256, 'route_entries': 256, 'selective_writes': False,
                       'reuse_code_checks': False, 'memory': 'pte'}}
        configurations['refill-4096'] = {
            'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 1,
            'cache_mib': 128, 'tlb_invalidation': 'full',
            'direct': {'mapping_entries': 4096, 'route_entries': 256, 'selective_writes': True,
                       'reuse_code_checks': False, 'memory': 'refill'}}
    if args.comparison in ('direct-flat', 'direct-code-watch', 'direct-cross-page', 'direct-routes'):
        configurations = {name: configurations[name] for name in ('mapping-4096', 'refill-4096')}
        configurations['flat'] = {
            'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 1,
            'cache_mib': 128, 'tlb_invalidation': 'full',
            'direct': {'mapping_entries': 256, 'route_entries': 256, 'selective_writes': False,
                       'reuse_code_checks': False, 'memory': 'flat'}}
    if args.comparison == 'direct-code-watch':
        configurations = {name + ('-watch' if enabled else '-compare'): {
            **configurations[name], 'direct': {**configurations[name]['direct'], 'code_watch': enabled}}
            for name in ('mapping-4096', 'flat') for enabled in (False, True)}
    if args.comparison == 'direct-cross-page':
        configurations = {name + ('-cross' if enabled else '-fallback'): {
            **configurations[name], 'direct': {**configurations[name]['direct'], 'code_watch': True, 'cross_page': enabled}}
            for name in ('mapping-4096', 'flat') for enabled in (False, True)}
    if args.comparison == 'direct-routes':
        base = configurations['flat']
        configurations = {
            name: {**base, 'direct': {**base['direct'], 'code_watch': True, 'cross_page': True,
                                    'route_entries': entries, 'route_epochs': epochs}}
            for name, entries, epochs in (
                ('reset-256', 256, False), ('reset-4096', 4096, False),
                ('epoch-256', 256, True), ('epoch-4096', 4096, True), ('epoch-65536', 65536, True))}
    for config in configurations.values():
        if config['execution'] == 'direct' and 'direct' not in config:
            config['direct'] = {'mapping_entries': 256, 'route_entries': 256, 'selective_writes': False, 'reuse_code_checks': False}
        if 'direct' in config:
            config['direct'].setdefault('memory', 'cached')
            config['direct'].setdefault('code_watch', False)
            config['direct'].setdefault('cross_page', False)
            config['direct'].setdefault('route_epochs', False)

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
                               NCE_DIRECT_SELECTIVE_WRITES=str(int(direct['selective_writes'])),
                               NCE_DIRECT_REUSE_CODE_CHECKS=str(int(direct['reuse_code_checks'])),
                               NCE_DIRECT_MEMORY=direct['memory'], NCE_DIRECT_CODE_WATCH=str(int(direct['code_watch'])),
                               NCE_DIRECT_CROSS_PAGE=str(int(direct['cross_page'])),
                               NCE_DIRECT_ROUTE_EPOCHS=str(int(direct['route_epochs'])))
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
