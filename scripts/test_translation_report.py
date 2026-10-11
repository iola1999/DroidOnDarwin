#!/usr/bin/env python3
"""用一次成功的双核运行记录检查报告缺项、采样统计与并行计时校验。"""
import argparse
from copy import deepcopy
import json
from pathlib import Path

from probe_lkl_translation import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    config = report['configuration']
    assert config['cpus'] == 2 and config['memory_interval_ms']
    original = report['runs'][0]['results']

    def accepted(rows):
        return validate(rows, config['memory_interval_ms'], config['mapping'], config['workload'],
                        config['tlb_invalidation'], config['execution'], config['direct'],
                        config['cpus'], config['parallel_profile'], config['uaccess_stop'],
                        code_memory=config.get('code_memory'))

    assert accepted(original)
    if config.get('code_memory'):
        assert not accepted([row for row in original if row.get('config') != 'translation_code_memory'])
        for field in ('kernel', 'direct'):
            rows = deepcopy(original)
            next(row for row in rows if row.get('config') == 'translation_code_memory')[field] = 'wrong-variant'
            assert not accepted(rows), field
    assert not accepted([row for row in original if row.get('cpu') != 1])
    cpu = next(row for row in original if row.get('config') == 'translation_execution')
    assert not accepted([*original[:-1], deepcopy(cpu), original[-1]])
    if config['parallel_profile']:
        assert not accepted([row for row in original if not row.get('parallel_execution_profile')])
        for field, value in (('thread_cpu_ns', 1), ('overlap_ns', 1 << 63), ('cpu1_ns', -1)):
            rows = deepcopy(original)
            next(row for row in rows if row.get('parallel_execution_profile'))[field] = value
            assert not accepted(rows), field
    if config['direct']['code_watch']:
        for field, value in (('scope', 'cpu'), ('queries', 1 << 63), ('protection_failures', 1)):
            rows = deepcopy(original)
            for row in rows:
                if row.get('direct_code_watch'):
                    row[field] = value
            assert not accepted(rows), field
        rows = deepcopy(original)
        next(row for row in rows if row.get('direct_code_watch'))['queries'] += 1
        assert not accepted(rows), 'inconsistent process totals'
    for metric in ('footprint_mean_bytes', 'footprint_sample_peak_bytes', 'footprint_latest_bytes'):
        rows = deepcopy(original)
        next(row for row in rows if 'memory_statistics' in row)[metric] += 1
        assert not accepted(rows), metric
    rows = deepcopy(original)
    next(row for row in rows if row.get('test') == 'translated_context_cleanup')['passed'] = False
    assert not accepted(rows)
    print('translation report: CPU records, enabled timing/watch counters, memory statistics and failed cleanup checks passed')


if __name__ == '__main__':
    main()
