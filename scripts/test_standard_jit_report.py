#!/usr/bin/env python3
"""用真实 JIT 记录检查缺项、重复、失败行与信号结果的解析。"""
import argparse
from copy import deepcopy
import json
from pathlib import Path

from probe_lkl_translation import validate_standard_jit


def check(rows, data_tbi=False, guest_page_bytes=16384):
    def accepted(values):
        return validate_standard_jit(values, data_tbi=data_tbi, guest_page_bytes=guest_page_bytes)

    assert accepted(rows)
    assert not validate_standard_jit(rows, data_tbi=not data_tbi, guest_page_bytes=guest_page_bytes)
    relevant = [row for row in rows if row.get('test') == 'linux_standard_jit' or row.get('config') == 'linux_standard_jit']
    rejected = 0
    for original in relevant:
        assert not accepted([row for row in rows if row is not original])
        assert not accepted([*rows, deepcopy(original)])
        rejected += 2
        for field in original:
            mutated = deepcopy(rows)
            index = next(i for i, row in enumerate(rows) if row is original)
            del mutated[index][field]
            assert not accepted(mutated), field
            rejected += 1
        index = next(i for i, row in enumerate(rows) if row is original)
        for field, value in original.items():
            if field in ('test', 'config', 'case'):
                changed = 'unexpected'
            elif field == 'passed':
                changed = False
            elif type(value) == int:
                changed = value + 1
            else:
                continue
            mutated = deepcopy(rows)
            mutated[index][field] = changed
            assert not accepted(mutated), (field, changed)
            rejected += 1
            if type(value) == int:
                mutated[index][field] = True
                assert not accepted(mutated), (field, 'bool')
                rejected += 1
    failed = deepcopy(relevant[-1])
    failed['passed'] = False
    assert not accepted([*rows, failed])
    return rejected + 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    result = json.loads(args.report.read_text())
    rejected = sum(check(run['results'], result['configuration'].get('data_tbi', False), result['configuration'].get('guest_page_bytes', 16384)) for run in result['runs'])
    print(f'standard JIT report: {len(result["runs"])} actual runs accepted, {rejected} invalid variants rejected')


if __name__ == '__main__':
    main()
