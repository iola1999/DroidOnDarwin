#!/usr/bin/env python3
"""用实际运行报告检查数据 TBI 配置、回退记录与拒绝的参数组合。"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys

from probe_lkl_translation import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', type=Path, nargs='+')
    args = parser.parse_args()
    count = 0
    for path in args.reports:
        report = json.loads(path.read_text())
        config = report['configuration']
        assert type(config['data_tbi']) is bool and config['cpus'] == 1
        assert config['mapping'] == 'pte' and config['workload'] == 'core'

        def accepted(rows):
            return validate(rows, config['memory_interval_ms'], config['mapping'], config['workload'],
                            config['tlb_invalidation'], config['execution'], config['direct'],
                            code_memory=config['code_memory'], online_cpus=1, guest_layout=config['guest_layout'], data_tbi=config['data_tbi'])

        for run in report['runs']:
            original = run['results']
            assert run['returncode'] == 0 and run['validated'] and accepted(original)
            if not config['data_tbi']:
                assert not any(row.get('test', '').startswith('translated_tagged_') for row in original)
                for field in ('data', 'unicorn_tagged_fills', 'alias_flushes'):
                    rows = deepcopy(original)
                    selected = next(row for row in rows if row.get('config') == 'translation_data_tbi') if field == 'data' else next(row for row in rows if row.get('translation_data_tbi'))
                    selected[field] = True if field == 'data' else 1
                    assert not accepted(rows)
                    count += 1
                continue
            for key, value in (('config', 'translation_data_tbi'), ('translation_data_tbi', True),
                               ('test', 'translated_data_tbi_and_unicorn_alias_invalidation'),
                               ('test', 'translated_tagged_syscall_abi_rejected'), ('test', 'translated_tagged_fetch_rejected')):
                selected = next(row for row in original if row.get(key) == value)
                assert not accepted([row for row in original if row is not selected])
                assert not accepted([*original, deepcopy(selected)])
                count += 2
            for field in ('data', 'fetch', 'tagged_syscall_abi'):
                for value in (None, 1, 'true', field != 'data'):
                    rows = deepcopy(original)
                    next(row for row in rows if row.get('config') == 'translation_data_tbi')[field] = value
                    assert not accepted(rows)
                    count += 1
            for field in ('unicorn_tagged_fills', 'alias_flushes'):
                for value in (0, -1, True, '1'):
                    rows = deepcopy(original)
                    next(row for row in rows if row.get('translation_data_tbi'))[field] = value
                    assert not accepted(rows)
                    count += 1
            if config['execution'] == 'direct':
                for instruction in (0xf8400949, 0xf8000949):
                    assert not accepted([row for row in original if row.get('direct_fallback') != instruction])
                    count += 1
    script = Path(__file__).with_name('probe_lkl_translation.py')
    for options, message in ((['--execution', 'direct', '--mapping', 'pte', '--direct-memory', 'view'], '用户数据 TBI 暂不支持 read-view'),
                             (['--cpus', '2'], '用户数据 TBI 当前仅验证单 CPU')):
        result = subprocess.run([sys.executable, script, '--build-only', '--data-tbi', '1', *options], capture_output=True, text=True, timeout=10)
        assert result.returncode == 2 and message in result.stderr
        count += 1
    # These invalid combinations stop before building and confirm the default
    # reaches the existing validation for SMP and the window mapping mode.
    for options, message in ((['--cpus', '2'], '双核原型需要'),
                             (['--execution', 'direct', '--direct-memory', 'view'], 'Direct 执行使用 PTE 映射')):
        result = subprocess.run([sys.executable, script, '--build-only', *options], capture_output=True, text=True, timeout=10)
        assert result.returncode == 2 and message in result.stderr and '用户数据 TBI' not in result.stderr
        count += 1
    print(f'translation data TBI: actual reports and {count} negative cases passed')


if __name__ == '__main__':
    main()
