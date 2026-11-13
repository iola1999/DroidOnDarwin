#!/usr/bin/env python3
"""以实际重叠报告检查地址身份、释放结果和末尾记录的拒绝边界。"""
from copy import deepcopy
import json
from pathlib import Path
import sys

from test_translation_address_domains import validate_domains


def main():
    report = json.loads(Path(sys.argv[1]).read_text())
    config = report['configuration']
    fixtures = report.get('exec_aslr_fixtures')
    rejected = 0
    for run in report['runs']:
        rows = run['results']
        assert run['validated'] and validate_domains(rows, config, exec_aslr_fixtures=fixtures)
        indexes = [i for i, row in enumerate(rows) if 'address_domain_region' in row or
                   row.get('test') == 'translated_host_guest_numeric_overlap' or
                   row.get('config') == 'translation_guest_layout']
        for index in indexes:
            variants = [rows[:index] + rows[index + 1:], rows[:index] + [deepcopy(rows[index])] + rows[index:]]
            for key, value in rows[index].items():
                missing = deepcopy(rows)
                del missing[index][key]
                variants.append(missing)
                if type(value) is int:
                    for bad in (-1, value + 1, str(value), True, 1 << 64):
                        if key in ('vmalloc_maps', 'vmalloc_revocations', 'ram_reclaims') and type(bad) is int and 0 <= bad < 1 << 64:
                            continue
                        changed = deepcopy(rows)
                        changed[index][key] = bad
                        variants.append(changed)
                if type(value) is bool:
                    for bad in (not value, int(value), str(value)):
                        changed = deepcopy(rows)
                        changed[index][key] = bad
                        variants.append(changed)
            for changed in variants:
                assert not validate_domains(changed, config, exec_aslr_fixtures=fixtures), (index, rows[index])
                rejected += 1
        assert not validate_domains(rows[:-3] + rows[-2:] + rows[-3:-2], config, exec_aslr_fixtures=fixtures)
        rejected += 1
    print(f'address-domain report: actual CLI/GCD accepted; {rejected} negative cases passed')


if __name__ == '__main__':
    main()
