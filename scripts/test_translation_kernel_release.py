#!/usr/bin/env python3
"""用实际 core/init 报告和原始失败日志检查页表释放告警。"""
import argparse
import hashlib
import json
from pathlib import Path

from probe_lkl_translation import validate as validate_core
from probe_stock_init import validate as validate_init


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-report', type=Path, nargs='+', required=True)
    parser.add_argument('--init-report', type=Path, required=True)
    parser.add_argument('--failure-log', type=Path, required=True)
    args = parser.parse_args()
    failure = args.failure_log.read_text()
    assert 'BUG: non-zero pgtables_bytes on freeing mm:' in failure
    accepted = rejected = 0
    for path in args.core_report:
        report = json.loads(path.read_text())
        c = report['configuration']
        for run in report['runs']:
            log_path = path.parent / f'{run["entry"]}-{run["iteration"]}.log'
            assert hashlib.sha256(log_path.read_bytes()).hexdigest() == run['stderr_sha256']

            def valid(log):
                return validate_core(run['results'], c['memory_interval_ms'], c['mapping'], c['workload'],
                                     c['tlb_invalidation'], c['execution'], c['direct'], c['cpus'],
                                     c['parallel_profile'], c['uaccess_stop'], code_memory=c['code_memory'],
                                     online_cpus=c['online_cpus'], guest_layout=c['guest_layout'],
                                     guest_page_bytes=c['guest_page_bytes'], data_tbi=c['data_tbi'], kernel_log=log,
                                     exec_aslr_fixtures=report.get('exec_aslr_fixtures'))

            assert run['validated'] and valid(log_path.read_text())
            assert not valid(failure)
            accepted += 1
            rejected += 1
    report = json.loads(args.init_report.read_text())
    prepared = report['input']
    options = {'guest_page_bytes': report['source_configuration'].get('guest_page_bytes', 16384),
               'original_input': prepared if prepared.get('sdk_partitions') or prepared.get('hyperos_manifest') else None,
               'exec_elf_images': prepared.get('exec_elf_images')}
    for run in report['runs']:
        assert run['observation_validated']
        assert validate_init(run['results'], run['mode'], True, kernel_log=run['stderr'], **options)
        assert not validate_init(run['results'], run['mode'], True, kernel_log=failure, **options)
        accepted += 1
        rejected += 1
    print(json.dumps({'test': 'kernel_page_table_release', 'passed': True,
                      'accepted_runs': accepted, 'original_warning_rejections': rejected,
                      'failure_log_sha256': hashlib.sha256(args.failure_log.read_bytes()).hexdigest()}))


if __name__ == '__main__':
    main()
