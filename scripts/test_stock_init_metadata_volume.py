#!/usr/bin/env python3
"""用实际两页报告核对 metadata 的设备、持久化、原目录与清理边界。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True

from probe_stock_init import ROOT, validate
from stock_init_metadata_volume import rc_results
from test_stock_init_stop import index_of


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def accepted(rows, run, report, prepared=None):
    if not validate(rows, run['mode'], True,
                    guest_page_bytes=report['source_configuration'].get('guest_page_bytes', 16384),
                    kernel_log=run['stderr'], original_input=prepared or report['input']):
        return False
    observed = rc_results(rows)
    return ((run['mode'] != 'original' or observed['apex_sessions_opened']) and
            (run['phase'] != 'recovery' or observed['directories_match_original_rc'] is True))


def variants(rows, mode, phase):
    keys = ('stock_init_metadata_partition', 'stock_init_metadata_mount', 'stock_init_metadata_open',
            'stock_init_metadata_retained', 'stock_init_metadata_generation',
            'stock_init_metadata_directory_sync', 'stock_init_metadata_cleanup')
    for key in keys:
        index = index_of(rows, key)
        yield 'missing_' + key, rows[:index] + rows[index + 1:]
    yield 'missing_metadata_directories', [row for row in rows if 'stock_init_metadata_directory' not in row]
    changes = [('stock_init_metadata_partition', 'start_sector', 2049),
               ('stock_init_metadata_partition', 'sectors', 32767),
               ('stock_init_metadata_partition', 'bytes', 16777217),
               ('stock_init_metadata_partition', 'name', 'vendor'),
               ('stock_init_metadata_partition', 'device', 254 << 32 | 2),
               ('stock_init_metadata_partition', 'readonly', True),
               ('stock_init_metadata_partition', 'superblock_equal', False),
               ('stock_init_metadata_partition', 'end_read_result', 1),
               ('stock_init_metadata_partition', 'beyond_read_result', 1),
               ('stock_init_metadata_mount', 'flags', 0),
               ('stock_init_metadata_open', 'fd', 2),
               ('stock_init_metadata_retained', 'cloexec', False),
               ('stock_init_metadata_generation', 'file_fsync', -5),
               ('stock_init_metadata_generation', 'directory_fsync', -5),
               ('stock_init_metadata_generation', 'inherited_fd_closed', False),
               ('stock_init_metadata_directory_sync', 'fd', 0),
               ('stock_init_metadata_directory_sync', 'result', False),
               ('stock_init_metadata_cleanup', 'sync_result', -5),
               ('stock_init_metadata_cleanup', 'close_result', -5)]
    if mode == 'original':
        changes += [('stock_init_metadata_cleanup', 'held_stat_result', 0),
                    ('stock_init_metadata_cleanup', 'path_stat_result', -5),
                    ('stock_init_metadata_cleanup', 'held_device', 259 << 32 | 2)]
    else:
        changes += [('stock_init_metadata_cleanup', 'held_stat_result', -13),
                    ('stock_init_metadata_cleanup', 'unmount_result', -1)]
    for key, field, value in changes:
        changed = deepcopy(rows)
        changed[index_of(changed, key)][field] = value
        yield 'changed_' + key + '_' + field, changed
    closed = next(index for index, row in enumerate(rows) if row.get('host_disk_close') and row['slot'] == 2)
    for field, value in (('sync_attempted', False), ('sync_result', False), ('close_result', -5)):
        changed = deepcopy(rows)
        changed[closed][field] = value
        yield 'changed_metadata_host_close_' + field, changed
    directories = [index for index, row in enumerate(rows) if 'stock_init_metadata_directory' in row]
    changed = deepcopy(rows)
    changed[directories[0]]['path'] = '/data'
    yield 'changed_metadata_directory_path', changed
    changed = deepcopy(rows)
    changed[directories[0]]['label_hex'] = ' '
    yield 'malformed_metadata_label_hex', changed
    changed = deepcopy(rows)
    del changed[directories[0]]['label_result']
    yield 'missing_metadata_directory_label_result', changed
    if phase == 'recovery':
        changed = deepcopy(rows)
        changed[directories[-1]]['label_hex'] = changed[directories[0]]['label_hex']
        changed[directories[-1]]['label_result'] = changed[directories[0]]['label_result']
        yield 'changed_recovered_sessions_label', changed
        changed = deepcopy(rows)
        changed[directories[-1]]['gid'] = 0
        yield 'changed_recovered_sessions_gid', changed
    if mode == 'original':
        changed = deepcopy(rows)
        for row in changed:
            if row.get('stock_init_block') == 56 and row['paths_hex'] == ['/metadata/apex/sessions'.encode().hex()]:
                row['result'] = -13
        yield 'failed_all_original_sessions_opens', changed
    moves = [('stock_init_metadata_partition', 'after', 'stock_init_metadata_mount'),
             ('stock_init_metadata_directory_sync', 'before', 'stock_init_host_stop_reaped'),
             ('stock_init_metadata_cleanup', 'before', 'stock_init_host_stop_reaped'),
             ('stock_init_metadata_cleanup', 'after', 'stock_init_data_cleanup'),
             ('stock_init_metadata_cleanup', 'after', 'stock_init_shutdown')]
    for key, relation, anchor in moves:
        changed = deepcopy(rows)
        moved = changed.pop(index_of(changed, key))
        changed.insert(index_of(changed, anchor) + (relation == 'after'), moved)
        yield key + '_' + relation + '_' + anchor, changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--historical-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('--output 需要新文件')
    paths = [args.report.resolve(), args.historical_report.resolve(), Path(__file__).resolve(),
             ROOT / 'scripts/probe_stock_init.py', ROOT / 'scripts/stock_init_data.py',
             ROOT / 'scripts/stock_init_metadata_volume.py', ROOT / 'scripts/stock_init_stop.py']
    result = {'test': 'stock_init_metadata_volume_report_contract', 'passed': False, 'cases': [],
              'report': str(paths[0]), 'historical_report': str(paths[1]),
              'scope': '实际报告与删除、改值、顺序变体；不启动客体或读取文件内容。',
              'sha256': {str(path): sha(path) for path in paths}}
    try:
        report = json.loads(paths[0].read_text())
        if (report.get('metadata_persistence_validated') is not True or report.get('metadata_rc_validated') is not True or
                report.get('metadata_original_unchanged') is not True):
            raise ValueError('需要完成同盘恢复、原目录和 APEX sessions 验收的实际报告')
        expected = {(phase, entry) for phase in ('lifecycle', 'original', 'recovery') for entry in ('cli', 'gcd')}
        if len(report['runs']) != 6 or {(run['phase'], run['entry']) for run in report['runs']} != expected:
            raise ValueError('需要完整的六入口')
        previous = None
        for run in report['runs']:
            actual = accepted(run['results'], run, report)
            result['cases'].append({'phase': run['phase'], 'entry': run['entry'], 'case': 'actual', 'passed': actual})
            generation = next(row for row in run['results'] if 'stock_init_metadata_generation' in row)
            if previous is not None and generation['previous'] != previous:
                raise ValueError('实际 generation 不连续')
            previous = generation['generation']
            for name, rows in variants(run['results'], run['mode'], run['phase']):
                case = {'phase': run['phase'], 'entry': run['entry'], 'case': name, 'passed': False}
                try:
                    case['accepted'] = accepted(rows, run, report)
                    case['passed'] = case['accepted'] is False
                except (KeyError, TypeError, ValueError, IndexError) as error:
                    case['error'] = str(error)
                result['cases'].append(case)
            malformed = deepcopy(run['results'])
            directory = next(row for row in malformed if 'stock_init_metadata_directory' in row)
            del directory['label_result']
            invalid = accepted(malformed, run, report) is False
            observed = rc_results(malformed)
            preserved = json.loads(json.dumps({'results': malformed, 'observation_validated': False, 'metadata_rc': observed}))
            result['cases'].append({'phase': run['phase'], 'entry': run['entry'], 'case': 'malformed_rc_failure_preserved',
                                    'passed': invalid and preserved['metadata_rc'].get('error') == 'metadata 目录观察字段无效' and
                                              preserved['metadata_rc']['directories_match_original_rc'] is None})
            altered = deepcopy(report['input'])
            altered['metadata_image']['partition']['start_sector'] += 1
            result['cases'].append({'phase': run['phase'], 'entry': run['entry'], 'case': 'changed_input_partition_contract',
                                    'passed': accepted(run['results'], run, report, altered) is False})
        if previous != report['metadata_final_generation']:
            raise ValueError('最终 generation 与实际记录不同')
        historical = json.loads(paths[1].read_text())
        if historical['input'].get('metadata_image'):
            raise ValueError('历史兼容报告需要保留无 metadata 的输入契约')
        result['historical_checks'] = [{'phase': run['phase'], 'entry': run['entry'],
                                       'passed': validate(run['results'], run['mode'], True,
                                                          guest_page_bytes=historical['source_configuration'].get('guest_page_bytes', 16384),
                                                          kernel_log=run['stderr'], original_input=historical['input'])}
                                      for run in historical['runs']]
        result['inputs_unchanged'] = all(sha(Path(path)) == digest for path, digest in result['sha256'].items())
        result['passed'] = result['inputs_unchanged'] and all(case['passed'] for case in result['cases'] + result['historical_checks'])
    except (OSError, KeyError, TypeError, ValueError, IndexError) as error:
        result['error'] = str(error)
    output.parent.mkdir(parents=True, exist_ok=True)
    result['case_count'] = len(result['cases'])
    result['failed_cases'] = [case for case in result['cases'] if not case['passed']]
    with output.open('x') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    print(json.dumps({'passed': result['passed'], 'cases': result['case_count'], 'result': str(output)}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
