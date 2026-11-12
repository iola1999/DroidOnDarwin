#!/usr/bin/env python3
"""用实际原 init 数据报告检查停止、回收与清理顺序的解析边界。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True

from probe_stock_init import ROOT, validate
from stock_init_stop import validate as validate_stop

HISTORICAL = ROOT / 'experiments/native-linux/results-stock-init-main-integration-20261112.json'
STOP_KEYS = ('stock_init_stop_request', 'stock_init_host_stop', 'stock_init_host_stop_reaped',
             'stock_init_host_stop_boundaries', 'stock_init_owned_ordinary', 'stock_init_ordinary_ready')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def index_of(rows, key):
    found = [index for index, row in enumerate(rows) if key in row]
    if len(found) != 1:
        raise ValueError('实际记录数量无效：' + key)
    return found[0]


def variants(rows, mode):
    observation = rows[index_of(rows, 'stock_init_observation')]
    pid = observation['ancestor_pid']
    removed = ('stock_init_stop_request', 'stock_init_host_stop_reaped',
               'stock_init_observation', 'stock_init_data_cleanup')
    if mode == 'lifecycle':
        removed += ('stock_init_host_stop_boundaries', 'stock_init_owned_ordinary', 'stock_init_ordinary_ready')
    if any('stock_init_host_stop' in row for row in rows):
        removed += ('stock_init_host_stop',)
    for key in removed:
        index = index_of(rows, key)
        yield 'missing_' + key, rows[:index] + rows[index + 1:]
    yield 'missing_all_stop_records', [row for row in rows if not any(key in row for key in STOP_KEYS)]

    changes = [('stock_init_stop_request', 'ancestor_pid', pid + 1),
               ('stock_init_stop_request', 'result', -5),
               ('stock_init_host_stop_reaped', 'ancestor_pid', pid + 1),
               ('stock_init_host_stop_reaped', 'result', 0),
               ('stock_init_observation', 'waited', 0),
               ('stock_init_observation', 'waited', pid + 1)]
    if mode == 'lifecycle':
        owned = rows[index_of(rows, 'stock_init_owned_ordinary')]
        boundaries = rows[index_of(rows, 'stock_init_host_stop_boundaries')]
        changes += [('stock_init_owned_ordinary', 'ready_pid', owned['pid'] + 1),
                    ('stock_init_owned_ordinary', 'host_stop_result', 0),
                    ('stock_init_owned_ordinary', 'kill_result', -5),
                    ('stock_init_owned_ordinary', 'waited', 0),
                    ('stock_init_owned_ordinary', 'status', 0),
                    ('stock_init_host_stop_boundaries', 'peer_tid', boundaries['owner_tid']),
                    ('stock_init_host_stop_boundaries', 'peer_result', 0),
                    ('stock_init_ordinary_ready', 'guest_stop_errno', 1)]
    if any('stock_init_host_stop' in row for row in rows):
        changes += [('stock_init_host_stop', 'ancestor_pid', pid + 1),
                    ('stock_init_host_stop', 'result', -5),
                    ('stock_init_host_stop', 'result', False)]
    for key, field, value in changes:
        changed = list(rows)
        index = index_of(changed, key)
        changed[index] = deepcopy(changed[index])
        changed[index][field] = value
        yield 'changed_' + key + '_' + field + '_' + str(value), changed

    moves = [('stock_init_stop_request', 'after', 'stock_init_observation'),
             ('stock_init_host_stop_reaped', 'before', 'stock_init_observation'),
             ('stock_init_data_cleanup', 'before', 'stock_init_host_stop_reaped')]
    if mode == 'lifecycle':
        moves += [('stock_init_host_stop_boundaries', 'after', 'stock_init_owned_ordinary'),
                  ('stock_init_owned_ordinary', 'after', 'stock_init_stop_request')]
    if any('stock_init_host_stop' in row for row in rows):
        moves += [('stock_init_host_stop', 'before', 'stock_init_stop_request'),
                  ('stock_init_host_stop', 'after', 'stock_init_observation')]
    for key, relation, anchor in moves:
        changed = list(rows)
        moved = changed.pop(index_of(changed, key))
        index = index_of(changed, anchor) + (relation == 'after')
        changed.insert(index, moved)
        yield key + '_' + relation + '_' + anchor, changed


def check_history(path):
    archive = json.loads(path.read_text())
    report = archive.get('init', archive)
    if 'namespace_stop_contract' in report.get('input', {}):
        raise ValueError('历史兼容来源需要保留原停止契约')
    checks = []
    for run in report['runs']:
        if run.get('returncode') != 0 or run.get('observation_validated') is not True:
            continue
        rows = run.get('results', run.get('observation'))
        if not rows or any(key in row for key in STOP_KEYS[1:] for row in rows):
            raise ValueError('历史兼容来源包含新版停止记录或缺少实际记录')
        observation = rows[index_of(rows, 'stock_init_observation')]
        checks.append({'entry': run['entry'], 'mode': run['mode'],
                       'passed': validate_stop(rows, observation, run['mode'])})
    if not checks or not all(check['passed'] for check in checks):
        raise ValueError('真实历史停止契约校验失败')
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--historical-report', type=Path, default=HISTORICAL)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source, historical, output = args.report.resolve(), args.historical_report.resolve(), args.output.resolve()
    if output in (source, historical) or output.exists():
        parser.error('--output 需要新的结果文件')
    result = {'test': 'stock_init_stop_report_contract', 'passed': False, 'cases': [],
              'scope': '仅检查实际报告与删除、改值、顺序变体；未重跑客体或访问数据盘。',
              'report': str(source), 'historical_report': str(historical)}
    try:
        paths = (source, historical, Path(__file__).resolve(), ROOT / 'scripts/probe_stock_init.py',
                 ROOT / 'scripts/stock_init_stop.py', ROOT / 'scripts/stock_init_data.py',
                 ROOT / 'scripts/stock_init_sdk.py', ROOT / 'scripts/probe_lkl_translation.py')
        result['sha256'] = {str(path): sha(path) for path in paths}
        report = json.loads(source.read_text())
        prepared = report['input']
        if (type(prepared.get('namespace_stop_contract')) is not int or prepared['namespace_stop_contract'] != 1 or
                not prepared.get('data_image') or report.get('data_persistence_validated') is not True):
            raise ValueError('需要已完成原 init 数据盘验收的新版实际报告')
        expected = {(phase, entry) for phase in ('lifecycle', 'original', 'recovery') for entry in ('cli', 'gcd')}
        actual = [(run['phase'], run['entry']) for run in report['runs']]
        if len(actual) != len(expected) or set(actual) != expected:
            raise ValueError('实际报告缺少六入口或包含重复入口')
        page_bytes = report['source_configuration'].get('guest_page_bytes', 16384)
        host_stops = 0
        for run in report['runs']:
            mode, phase, entry = run['mode'], run['phase'], run['entry']
            if (mode != ('original' if phase == 'original' else 'lifecycle') or run['returncode'] != 0 or
                    run['observation_validated'] is not True or
                    not validate(run['results'], mode, True, guest_page_bytes=page_bytes,
                                 kernel_log=run['stderr'], original_input=prepared)):
                raise ValueError('实际入口校验失败：' + phase + '/' + entry)
            result['cases'].append({'phase': phase, 'entry': entry, 'case': 'actual_entry', 'passed': True})
            host_stops += sum('stock_init_host_stop' in row for row in run['results'])
            for name, rows in variants(run['results'], mode):
                case = {'phase': phase, 'entry': entry, 'case': name, 'passed': False}
                try:
                    case['accepted'] = validate(rows, mode, True, guest_page_bytes=page_bytes,
                                                kernel_log=run['stderr'], original_input=prepared)
                    case['passed'] = case['accepted'] is False
                except (KeyError, TypeError, IndexError, ValueError) as error:
                    case['error'] = str(error)
                result['cases'].append(case)
        if not host_stops:
            raise ValueError('实际报告缺少用户权限拒绝后的宿主停止结果')
        result['historical_checks'] = check_history(historical)
        result['inputs_unchanged'] = all(sha(Path(path)) == digest for path, digest in result['sha256'].items())
        result['passed'] = result['inputs_unchanged'] and all(case['passed'] for case in result['cases'])
    except (OSError, KeyError, TypeError, IndexError, ValueError) as error:
        result['error'] = str(error)
    result['case_count'] = len(result['cases'])
    result['failed_case_count'] = sum(not case['passed'] for case in result['cases'])
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    print(json.dumps({'passed': result['passed'], 'result': str(output), 'cases': result['case_count'],
                      'failed_cases': result['failed_case_count']}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
