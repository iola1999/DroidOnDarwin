#!/usr/bin/env python3
"""交替比较相同 Android 负载的执行器与缓存设置，保留阶段内存采样。"""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys

from probe_lkl_translation import ROOT, WORK, sha, validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repeat', type=int, default=2)
    parser.add_argument('--comparison', choices=('cache', 'tlb', 'execution', 'direct-mappings', 'direct-cache', 'direct-memory', 'direct-flat', 'direct-code-watch', 'direct-cross-page', 'direct-routes', 'direct-retain-routes', 'direct-memory-groups', 'direct-read-views', 'direct-native-exclusive', 'direct-smp-uaccess', 'direct-vector-table', 'direct-smp-code-cache', 'direct-smp-code-watch', 'direct-linux-fastpath', 'direct-smp-linux-fastpath', 'direct-smp-online'), default='cache')
    parser.add_argument('--code-memory', choices=('map-jit', 'dual'), default='map-jit')
    parser.add_argument('--workload', choices=('android', 'services', 'desktop'), default='android')
    parser.add_argument('--entry', choices=('both', 'cli', 'gcd', 'window'), default='both')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--reuse-engine-build', action='store_true')
    parser.add_argument('--prepared-report', type=Path, help='在线 CPU 对照复用已通过的同负载 SMP 报告及二进制')
    args = parser.parse_args()
    if args.repeat < 2 or args.repeat % 2:
        parser.error('repeat 必须为至少 2 的偶数，以平衡执行顺序')
    if args.entry == 'window' and args.workload != 'desktop':
        parser.error('window 入口需要 desktop 负载')
    entries = ('cli', 'gcd') if args.entry == 'both' else (args.entry,)
    suffix = '' if args.comparison == 'cache' else '-' + args.comparison
    out = (args.output or WORK / f'android-translation{suffix}-matrix').resolve()
    if not out.is_relative_to(WORK.resolve()):
        parser.error('output 必须位于 work/native-linux-nce 下')
    engines = ('unicorn', 'direct') if args.comparison == 'execution' else ('direct',) if args.comparison.startswith('direct-') else ('unicorn',)
    folders = {name: out / ('runtime-' + name if args.comparison == 'execution' else 'runtime') for name in engines}
    cached = None
    if args.prepared_report:
        source_report = args.prepared_report.resolve()
        if args.comparison != 'direct-smp-online' or not source_report.is_relative_to(WORK.resolve()):
            parser.error('报告复用仅用于 direct-smp-online，报告需要位于 work/native-linux-nce')
        cached = json.loads(source_report.read_text())
        config = cached['configuration']
        if (config['workload'] != args.workload or config['execution'] != 'direct' or
                config['code_memory'] != args.code_memory or config['cpus'] != 2 or
                config.get('online_cpus', 2) != 2 or config['exec_cache'] != 'pages' or
                config['mapping'] != 'pte' or config['preemption'] != 'timer' or
                config['instruction_batch'] != 50000 or config['memory_interval_ms'] != 100 or
                not set(entries) <= set(config['entries']) or
                not set(entries) <= {row['entry'] for row in cached['runs']} or
                not all(row['validated'] and row['returncode'] == 0 for row in cached['runs'])):
            parser.error('复用报告的配置、入口或成功状态不匹配')
        folders = {'direct': source_report.parent}
        if out == source_report.parent:
            parser.error('新对照输出需要与复用报告目录分开')
        if any(str((source_report.parent / entry).relative_to(ROOT)) not in cached['sha256'] for entry in entries):
            parser.error('所需入口二进制未包含在复用报告的输入摘要中')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'summary.json').unlink(missing_ok=True)
    prepared = {}
    for execution, folder in folders.items():
        if cached is not None:
            prepared[execution] = cached
            continue
        command = [sys.executable, ROOT / 'scripts/probe_lkl_translation.py', '--mapping', 'pte',
                   '--preemption', 'timer', '--workload', args.workload, '--entry', args.entry, '--output', folder, '--execution', execution,
                   '--code-memory', args.code_memory]
        if prepared or args.reuse_engine_build:
            command += ['--reuse-engine-build']
        if args.comparison != 'cache':
            command += ['--cache-mib', '128', '--linux-fastpath', '1']
        if args.comparison in ('direct-smp-uaccess', 'direct-vector-table', 'direct-smp-code-cache', 'direct-smp-code-watch', 'direct-smp-linux-fastpath', 'direct-smp-online'):
            command += ['--cpus', '2', '--direct-memory', 'flat', '--direct-native-exclusive', '1',
                        '--direct-cross-page', '1', '--exec-cache', 'pages', '--tlb-invalidation', 'pages',
                        '--linux-fastpath', '0']
        if args.comparison in ('direct-vector-table', 'direct-smp-code-cache', 'direct-smp-code-watch', 'direct-smp-linux-fastpath', 'direct-smp-online'):
            command += ['--smp-uaccess-stop', 'code']
        if args.comparison in ('direct-linux-fastpath', 'direct-smp-linux-fastpath', 'direct-smp-online'):
            command += ['--direct-memory', 'flat', '--direct-native-exclusive', '1', '--direct-cross-page', '1',
                        '--direct-code-watch', '1', '--exec-cache', 'pages', '--tlb-invalidation', 'pages', '--linux-fastpath', '0']
        with (out / f'prepare-{execution}.log').open('w') as log:
            subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT, check=True, timeout=max(600, 480 * len(entries) + 120) if args.workload == 'desktop' else 600)
        prepared[execution] = json.loads((folder / 'summary.json').read_text())
    baseline = next(iter(prepared.values()))
    hashes = {str(Path(__file__).resolve().relative_to(ROOT)): sha(Path(__file__))}
    if cached is not None:
        hashes[str(source_report.relative_to(ROOT))] = sha(source_report)
    for result in prepared.values():
        for name, digest in result['sha256'].items():
            if name in hashes and hashes[name] != digest:
                raise RuntimeError('执行器构建期间输入发生变化：' + name)
            hashes[name] = digest
    common = {key: value for key, value in baseline['configuration'].items()
              if key not in ('execution', 'direct', 'cache_invalidation', 'linux_fastpath', 'translation_cache_bytes', 'tlb_invalidation')}
    common.update(entries=list(entries), parallel_profile=False)
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
    if args.comparison in ('direct-memory', 'direct-flat', 'direct-code-watch', 'direct-cross-page', 'direct-routes', 'direct-retain-routes', 'direct-memory-groups', 'direct-read-views', 'direct-native-exclusive'):
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
    if args.comparison in ('direct-flat', 'direct-code-watch', 'direct-cross-page', 'direct-routes', 'direct-retain-routes', 'direct-memory-groups', 'direct-read-views', 'direct-native-exclusive'):
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
    if args.comparison == 'direct-retain-routes':
        base = configurations['flat']
        configurations = {
            name: {**base, 'direct': {**base['direct'], 'code_watch': True, 'cross_page': True,
                                    'route_entries': entries, 'route_epochs': epochs, 'retain_routes': retain}}
            for name, entries, epochs, retain in (
                ('reset-256', 256, False, False), ('epoch-256', 256, True, False),
                ('retain-256', 256, True, True), ('retain-4096', 4096, True, True),
                ('retain-65536', 65536, True, True))}
    if args.comparison == 'direct-memory-groups':
        base = configurations['flat']
        configurations = {name: {**base, 'direct': {**base['direct'], 'code_watch': True,
                                                   'cross_page': True, 'memory_groups': enabled}}
                          for name, enabled in (('individual', False), ('grouped', True))}
    if args.comparison == 'direct-read-views':
        base = configurations['flat']
        configurations = {name: {**base, 'direct': {**base['direct'], 'code_watch': True,
                                                   'cross_page': True, 'memory': name}}
                          for name in ('flat', 'view')}
    if args.comparison == 'direct-native-exclusive':
        base = configurations['flat']
        configurations = {name: {**base, 'direct': {**base['direct'], 'code_watch': True,
                                                   'cross_page': True, 'native_exclusive': enabled}}
                          for name, enabled in (('cas', False), ('native', True))}
    if args.comparison == 'direct-smp-uaccess':
        configurations = {name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 0,
                                 'cache_mib': 128, 'tlb_invalidation': 'pages', 'uaccess_stop': name,
                                 'direct': baseline['configuration']['direct']}
                          for name in ('all', 'code')}
    if args.comparison == 'direct-vector-table':
        configurations = {name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 0,
                                 'cache_mib': 128, 'tlb_invalidation': 'pages', 'uaccess_stop': 'code',
                                 'direct': {**baseline['configuration']['direct'], 'vector_table': enabled}}
                          for name, enabled in (('fallback', False), ('table', True))}
    if args.comparison == 'direct-smp-code-cache':
        configurations = {
            name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 0,
                   'cache_mib': 128, 'tlb_invalidation': 'pages', 'uaccess_stop': 'code',
                   'direct': {**baseline['configuration']['direct'], 'reuse_code_checks': reuse,
                              'route_epochs': epochs, 'route_entries': entries}}
            for name, reuse, epochs, entries in (('baseline', False, False, 256),
                                                ('reuse', True, False, 256),
                                                ('epochs', True, True, 256),
                                                ('routes', True, True, 4096))}
    if args.comparison == 'direct-smp-code-watch':
        configurations = {name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 0,
                                 'cache_mib': 128, 'tlb_invalidation': 'pages', 'uaccess_stop': 'code',
                                 'direct': {**baseline['configuration']['direct'], 'code_watch': enabled}}
                          for name, enabled in (('compare', False), ('watch', True))}
    if args.comparison in ('direct-linux-fastpath', 'direct-smp-linux-fastpath'):
        configurations = {name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': enabled,
                                 'cache_mib': 128, 'tlb_invalidation': 'pages', 'uaccess_stop': baseline['configuration']['uaccess_stop'],
                                 'direct': baseline['configuration']['direct']}
                          for name, enabled in (('per-access', 0), ('block-entry', 1))}
    if args.comparison == 'direct-smp-online':
        configurations = {name: {'execution': 'direct', 'cache_invalidation': 'pages', 'linux_fastpath': 0,
                                 'cache_mib': 128, 'tlb_invalidation': 'pages', 'uaccess_stop': 'code',
                                 'online_cpus': count, 'direct': baseline['configuration']['direct']}
                          for name, count in (('online-1', 1), ('online-2', 2))}
        common.pop('online_cpus', None)
    for config in configurations.values():
        config.setdefault('online_cpus', common['cpus'])
        config['scheduler_slice_us'] = 750 if args.comparison == 'direct-smp-online' else 0
        if config['execution'] == 'direct' and 'direct' not in config:
            config['direct'] = {'mapping_entries': 256, 'route_entries': 256, 'selective_writes': False, 'reuse_code_checks': False}
        if 'direct' in config:
            config['direct'].setdefault('memory', 'cached')
            config['direct'].setdefault('code_watch', False)
            config['direct'].setdefault('cross_page', False)
            config['direct'].setdefault('route_epochs', False)
            config['direct'].setdefault('retain_routes', False)
            config['direct'].setdefault('memory_groups', False)
            config['direct'].setdefault('native_exclusive', False)
            config['direct'].setdefault('vector_table', False)

    def check_inputs():
        for path, expected in hashes.items():
            if sha(ROOT / path) != expected:
                raise RuntimeError('测量输入已变化: ' + path)

    check_inputs()
    runs = []
    for iteration in range(args.repeat):
        for entry in entries:
            order = list(configurations)
            if (iteration + (entry == 'gcd')) % 2:
                order.reverse()
            for name in order:
                config = configurations[name]
                folder = folders[config['execution']]
                env = dict(os.environ, NCE_ART_NTERP=str(common['art_nterp']),
                           NCE_ART_EXPANDED_JIT=str(common['art_expanded_jit']), NCE_ART_FAST_BASELINE=str(common['art_fast_baseline']),
                           NCE_ART_AOT='0', NCE_ART_BOOT_IMAGE='0', NCE_SCHED_SLICE_US=str(config['scheduler_slice_us']), NCE_TRANSLATION_FILES=str(folder), NCE_TRANSLATION_WORKLOAD=args.workload,
                           NCE_TRANSLATION_SYSTEM=str(WORK / 'processes/system.img'),
                           NCE_TRANSLATION_BATCH='50000', NCE_TRANSLATION_PREEMPTION='timer',
                           NCE_TRANSLATION_MEMORY_MS='100', NCE_TRANSLATION_CACHE=config['cache_invalidation'],
                           NCE_TRANSLATION_EXEC_CACHE=common['exec_cache'],
                           NCE_TRANSLATION_PARALLEL_PROFILE='0', NCE_TRANSLATION_ONLINE_CPUS=str(config['online_cpus']),
                           NCE_TRANSLATION_UACCESS_STOP=config.get('uaccess_stop', 'all'),
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
                               NCE_DIRECT_ROUTE_EPOCHS=str(int(direct['route_epochs'])),
                               NCE_DIRECT_RETAIN_ROUTES=str(int(direct['retain_routes'])),
                               NCE_DIRECT_MEMORY_GROUPS=str(int(direct['memory_groups'])),
                               NCE_DIRECT_NATIVE_EXCLUSIVE=str(int(direct['native_exclusive'])),
                               NCE_DIRECT_VECTOR_TABLE=str(int(direct['vector_table'])))
                for key in ('NCE_DESKTOP', 'NCE_SYSTEM_SERVER_PROBE', 'NCE_DIAGNOSTICS', 'NCE_TRACE_SYSTEMUI'):
                    env.pop(key, None)
                if args.workload == 'desktop':
                    env.update(DYLD_LIBRARY_PATH=str(ROOT / 'work/angle-macos/prefix/lib'), ANGLE_DEFAULT_PLATFORM='metal')
                stem = f'{iteration}-{entry}-{name}'
                data = None
                if args.workload in ('services', 'desktop'):
                    data_path = out / f'{stem}-data.img'
                    seed = ROOT / baseline['data_seed']['path']
                    subprocess.run(['cp', '-c', seed, data_path], check=True, timeout=60)
                    env['NCE_TRANSLATION_DATA'] = str(data_path)
                    data = {'path': str(data_path.relative_to(ROOT)), 'initial_sha256': sha(data_path), 'fresh': True}
                with (out / f'{stem}.jsonl').open('w') as stdout, (out / f'{stem}.log').open('w') as stderr:
                    try:
                        process = subprocess.run([folder / entry, WORK / ('lkl-translation-smp-kernel' if common['cpus'] == 2 else 'lkl-translation-kernel') / 'lkl-image.bin'],
                                                 env=env, stdout=stdout, stderr=stderr, timeout=480 if args.workload == 'desktop' else 120)
                        returncode = process.returncode
                    except subprocess.TimeoutExpired:
                        returncode = 'timeout'
                rows = [json.loads(line) for line in (out / f'{stem}.jsonl').read_text().splitlines() if line.startswith('{')]
                passed = returncode == 0 and validate(rows, 100, 'pte', args.workload, config['tlb_invalidation'], config['execution'], direct, common['cpus'], False, config.get('uaccess_stop', 'all'), code_memory=args.code_memory, online_cpus=config['online_cpus'])
                slices = [row for row in rows if row.get('config') == 'linux_scheduler_slice']
                passed = passed and len(slices) == 1 and slices[0]['requested_us'] == config['scheduler_slice_us']
                if config['scheduler_slice_us']:
                    passed = passed and slices[0]['applied_ns'] == config['scheduler_slice_us'] * 1000
                if data is not None:
                    data['final_sha256'] = sha(data_path)
                    metadata = subprocess.run(['/opt/homebrew/opt/e2fsprogs/sbin/dumpe2fs', '-h', data_path],
                                              capture_output=True, text=True, timeout=30)
                    data['clean_shutdown'] = (metadata.returncode == 0 and
                                              re.search(r'^Filesystem state:\s+clean$', metadata.stdout, re.M) is not None and
                                              'needs_recovery' not in metadata.stdout and 'orphan_present' not in metadata.stdout)
                    passed = passed and data['clean_shutdown']
                runs.append({'iteration': iteration, 'entry': entry, 'variant': name, 'results': rows,
                             'returncode': returncode, 'validated': passed, 'data_image': data,
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
