#!/usr/bin/env python3
"""从独立访存检查的实际报告构造负例，检查结果验证器拒绝配置和采样错误。"""
import argparse
import copy
import json
from pathlib import Path

from probe_direct_execution import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    runs = report['runs']
    expected = {(entry, f'memory-check-{backend}')
                for entry in ('cli', 'gcd') for backend in ('native', 'cached', 'flat')}
    assert len(runs) == len(expected) and {(run['entry'], run['mode']) for run in runs} == expected
    rejected = 0
    for run in runs:
        rows = run['results']
        mode, entry = run['mode'], run['entry']
        identity = next(row for row in rows if 'mode' in row)
        interval = identity['memory_interval_ms']
        assert validate(rows, mode, interval, entry=entry), (entry, mode, 'valid report')

        def reject(change):
            nonlocal rejected
            altered = copy.deepcopy(rows)
            change(altered)
            assert not validate(altered, mode, interval, entry=entry), (entry, mode, 'invalid report accepted')
            rejected += 1

        def identity_field(field, value):
            return lambda altered: next(row for row in altered if 'mode' in row).__setitem__(field, value)

        reject(identity_field('mode', 'memory-bench-' + mode.rsplit('-', 1)[1]))
        reject(identity_field('entry', 'gcd' if entry == 'cli' else 'cli'))
        reject(identity_field('memory_interval_ms', interval + 1))
        reject(lambda altered: altered.remove(next(row for row in altered if 'memory_kernel' in row)))
        reject(lambda altered: altered.insert(-1, copy.deepcopy(next(row for row in altered if 'memory_kernel' in row))))
        reject(lambda altered: next(row for row in altered if row.get('memory_benchmark_config')).__setitem__('page_bits', 12))
        reject(lambda altered: next(row for row in altered if 'memory_kernel' in row).__setitem__('iterations', 1))
        reject(lambda altered: next(row for row in altered if 'memory_kernel' in row).__setitem__('check_only', False))
        reject(lambda altered: next(row for row in altered if 'memory_kernel' in row).__setitem__('wall_ns', 1))
        reject(lambda altered: next(row for row in altered if 'memory_kernel' in row).__setitem__('thread_cpu_ns', 1))
        reject(lambda altered: altered.remove(next(row for row in altered if row.get('test') == 'memory_benchmark_same_va_private_shared_switch')))
        reject(lambda altered: next(row for row in altered if 'memory_kernel' in row).__setitem__('passed', False))
    print(json.dumps({'valid_runs': len(runs), 'rejected_mutations': rejected, 'passed': True}))


if __name__ == '__main__':
    main()
