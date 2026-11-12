#!/usr/bin/env python3
"""用实际 ELF 报告检查样本缺失、地址对齐、brk 边界与 auxv 归属。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path

from exec_aslr import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    results = []
    for path in args.reports:
        report = json.loads(path.read_text())
        fixtures = report['exec_aslr_fixtures']
        page = report['configuration']['guest_page_bytes']
        for run in report['runs']:
            rows = run['results']
            guest = next(row for row in rows if row.get('config') == 'translation_guest_layout')
            assert run['validated'] and validate(rows, fixtures, guest, page)
            indexes = [i for i, row in enumerate(rows) if row.get('test', '').startswith('linux_exec_aslr')]
            cases = [rows[:i] + rows[i + 1:] for i in indexes]
            sample = next(i for i, row in enumerate(rows) if row.get('fixture') == 'exec-near-end' and row.get('mode') == 'full' and 'linux_exec_aslr_sample' in row)
            for field, value in (('start_brk', guest['guest_end'] + page), ('brk', guest['guest_end'] + 1),
                                 ('sigreturn', guest['guest_base'] - page), ('phdr', 0), ('entry', 0), ('at_base', 0)):
                changed = deepcopy(rows)
                changed[sample][field] = value
                cases.append(changed)
            for key in ('program', 'interpreter'):
                changed = deepcopy(rows)
                changed[sample][key]['bias'] += 1
                cases.append(changed)
            loader = next(i for i, row in enumerate(rows) if row.get('fixture') == 'loader' and 'linux_exec_aslr_sample' in row)
            changed = deepcopy(rows)
            changed[loader]['phdr'] = changed[loader]['program']['bias'] + fixtures['images']['loader']['phdr_vaddr']
            cases.append(changed)
            cases.append(rows + [deepcopy(rows[sample])])
            assert all(not validate(changed, fixtures, guest, page) for changed in cases)
            assert not validate(rows, None, guest, page)
            results.append({'report': str(path), 'report_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                            'entry': run['entry'], 'guest_page_bytes': page, 'accepted': True,
                            'rejected_cases': len(cases) + 1})
    output = {'test': 'exec_aslr_report_contract', 'passed': True, 'runs': results,
              'source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps({'passed': True, 'accepted': len(results), 'rejected': sum(r['rejected_cases'] for r in results)}))


if __name__ == '__main__':
    main()
