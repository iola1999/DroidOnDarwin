#!/usr/bin/env python3
"""用实际 core/Bionic 报告检查 flat 刷新、映射表分配与回收。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path

from probe_lkl_translation import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    results = []
    for path in args.reports:
        report = json.loads(path.read_text())
        config = report['configuration']

        def accepted(rows):
            return validate(rows, config['memory_interval_ms'], config['mapping'], config['workload'],
                            config['tlb_invalidation'], config['execution'], config['direct'], config['cpus'],
                            config['parallel_profile'], config['uaccess_stop'], code_memory=config['code_memory'],
                            online_cpus=config['online_cpus'], guest_layout=config['guest_layout'],
                            data_tbi=config['data_tbi'], guest_page_bytes=config['guest_page_bytes'],
                            exec_aslr_fixtures=report.get('exec_aslr_fixtures'))

        for run in report['runs']:
            rows = run['results']
            assert run['returncode'] == 0 and accepted(rows)
            index = next(i for i, row in enumerate(rows) if 'direct_flat_tables' in row)
            cases = [rows[:index] + rows[index + 1:], rows + [deepcopy(rows[index])]]
            for field in ('cpu', 'enabled', 'live_virtual_bytes', 'peak_virtual_bytes', 'updates', 'refresh_pages'):
                missing = deepcopy(rows)
                del missing[index][field]
                cases.append(missing)
                for value in (None, '1', -1, True, 1 << 64):
                    if field == 'enabled' and value is True:
                        continue
                    changed = deepcopy(rows)
                    changed[index][field] = value
                    cases.append(changed)
            for field, value in (('cpu', 1000), ('enabled', False), ('live_virtual_bytes', 1), ('peak_virtual_bytes', 0),
                                 ('peak_virtual_bytes', 1), ('refresh_pages', 0)):
                changed = deepcopy(rows)
                changed[index][field] = value
                cases.append(changed)
            cleanup = next(i for i, row in enumerate(rows) if row.get('test') == 'translated_context_cleanup')
            for field, value in (('passed', False), ('freed', rows[cleanup]['freed'] - 1),
                                 ('allocated', 0), ('freed', None), ('freed', True)):
                changed = deepcopy(rows)
                changed[cleanup][field] = value
                cases.append(changed)
            assert all(not accepted(changed) for changed in cases)
            changed = deepcopy(rows)
            changed[index]['updates'] = 0
            assert accepted(changed)
            results.append({'report': str(path), 'report_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                            'entry': run['entry'], 'guest_page_bytes': config['guest_page_bytes'],
                            'accepted': True, 'rejected_cases': len(cases)})
    output = {'test': 'translation_flat_report_contract', 'passed': True, 'runs': results,
              'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps({'passed': True, 'accepted_runs': len(results), 'rejected_cases': sum(r['rejected_cases'] for r in results)}))


if __name__ == '__main__':
    main()
