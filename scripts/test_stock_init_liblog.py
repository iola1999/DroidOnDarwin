#!/usr/bin/env python3
"""编译实际 liblog 采集头，检查协议、捕获限额及可选的原 JSONL 回归。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
import stock_init_liblog as liblog

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'experiments/native-linux/translation/stock-init-liblog.h'
FIXTURE = HEADER.with_name('stock-init-liblog-test.c')
FILE_LIMIT = 4 << 20


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fixture_checks(name, rows, metadata):
    seen, recorded, omitted, rejected = {
        'protocol': (3, 3, 0, 21),
        'boundary': (9, 9, 0, 21),
        'limit': (520, 512, 8, 0),
    }[name]
    fatal = [row for row in rows if 'stock_init_liblog_fatal' in row]
    checks = [
        {'name': 'production_validator', 'passed': liblog.validate_liblog(rows, required=True)},
        {'name': 'fixture_metadata', 'passed': metadata == {
            'fixture': name, 'accepted': seen, 'rejected': rejected, 'guest_unchanged': True}},
        {'name': 'counts', 'passed': len(rows) == recorded + 1 and len(fatal) == recorded and
         tuple(rows[-1].get(key) for key in ('seen', 'recorded', 'omitted')) == (seen, recorded, omitted)},
    ]
    if name == 'protocol':
        checks.extend([
            {'name': 'full_write', 'passed': fatal[0]['write_result'] == 41 and
             bytes.fromhex(fatal[0]['message_hex']) == b'fixture fatal\0'},
            {'name': 'truncated_message', 'passed': fatal[1]['message_bytes'] == 3001 and
             fatal[1]['captured_bytes'] == 2048 and bytes.fromhex(fatal[1]['message_hex']) == b'x' * 2048},
            {'name': 'header_tid_wrap', 'passed': fatal[2]['tid'] == 65538 and fatal[2]['header_tid'] == 2},
        ])
    elif name == 'boundary':
        checks.extend([
            {'name': 'capture_boundary', 'passed':
             [(row['message_bytes'], row['captured_bytes']) for row in fatal[:2]] == [(2048, 2048), (2049, 2048)] and
             bytes.fromhex(fatal[0]['message_hex']) == b'x' * 2047 + b'\0' and
             bytes.fromhex(fatal[1]['message_hex']) == b'x' * 2048},
            {'name': 'payload_boundaries', 'passed':
             all(1 + row['tag_bytes'] + row['message_bytes'] == 4068 for row in fatal[2:4]) and
             fatal[2]['tag_hex'] == '00' and fatal[2]['write_result'] == 0 and
             fatal[2]['sec'] == (1 << 32) - 1 and fatal[2]['nsec'] == 999999999 and
             fatal[2]['header_tid'] == 65535 and fatal[2]['fd'] == 0 and
             fatal[3]['tag_bytes'] == 128 and fatal[3]['write_result'] == -4095},
            {'name': 'text_log_ids_and_short_write', 'passed':
             [row['log_id'] for row in fatal[4:]] == [0, 1, 3, 4, 7] and
             all(row['write_result'] == 1 for row in fatal[4:])},
        ])
    return checks


def validator_checks(rows, limited):
    checks = []

    def check(name, candidate, expected, required=True):
        actual = liblog.validate_liblog(candidate, required=required)
        checks.append({'name': name, 'expected': expected, 'actual': actual, 'passed': actual is expected})

    check('actual', rows, True)
    check('disabled_without_rows', [], True, required=False)
    check('required_without_rows', [], False)
    zero = deepcopy(rows[-1])
    zero.update(seen=0, recorded=0, omitted=0)
    check('required_zero_match_summary', [zero], True)
    check('missing_summary', rows[:-1], False)
    check('missing_samples', rows[-1:], False)
    check('duplicate_summary', [*rows, rows[-1]], False)
    check('summary_before_samples', [rows[-1], *rows[:-1]], False)
    check('extra_unrelated_rows', [{'config': 'fixture'}, *rows, {'test': 'fixture'}], True)
    for key, value in (
        ('stock_init_liblog_fatal', False), ('log_index', 2), ('pid', 0), ('tid', 0),
        ('header_tid', 4243), ('log_id', 2), ('sec', 1 << 32), ('nsec', 1000000000),
        ('fd', -1), ('write_result', -4096), ('write_result', 42),
        ('tag_bytes', 0), ('tag_bytes', 129), ('message_bytes', 4068),
        ('captured_bytes', 15), ('tag_hex', 'zz'), ('message_hex', ''),
    ):
        changed = deepcopy(rows)
        changed[0][key] = value
        check('invalid_fatal_' + key + '_' + str(value), changed, False)
    for key in ('log_index', 'pid', 'tid', 'header_tid', 'log_id', 'sec', 'nsec', 'fd',
                'write_result', 'tag_bytes', 'message_bytes', 'captured_bytes'):
        changed = deepcopy(rows)
        changed[0][key] = True
        check('boolean_fatal_' + key, changed, False)
    for key, value in (
        ('stock_init_liblog_summary', False), ('seen', 2), ('recorded', 2), ('omitted', 1),
        ('capture_limit_bytes', 2047), ('record_limit', 513), ('seen', 1 << 64),
    ):
        changed = deepcopy(rows)
        changed[-1][key] = value
        check('invalid_summary_' + key + '_' + str(value), changed, False)
    for key in ('seen', 'recorded', 'omitted', 'capture_limit_bytes', 'record_limit'):
        changed = deepcopy(rows)
        changed[-1][key] = True
        check('boolean_summary_' + key, changed, False)
    for key in ('tag_hex', 'message_hex'):
        for suffix, data in (('unterminated', b'x' * (rows[0]['tag_bytes' if key == 'tag_hex' else 'message_bytes'])),
                             ('internal_nul', b'\0' + bytes.fromhex(rows[0][key])[1:])):
            changed = deepcopy(rows)
            changed[0][key] = data.hex()
            check(key + '_' + suffix, changed, False)
    changed = deepcopy(rows)
    changed[1]['message_hex'] = '00' + changed[1]['message_hex'][2:]
    check('nul_inside_truncated_message', changed, False)
    changed = deepcopy(rows)
    changed[0]['tag_hex'] = changed[0]['tag_hex'].upper()
    check('noncanonical_hex', changed, False)
    changed = deepcopy(rows)
    changed[0]['trace_index'] = changed[0].pop('log_index')
    check('historical_trace_index', changed, False)
    changed = deepcopy(rows)
    changed[0]['result'] = changed[0]['write_result']
    check('result_field_conflict', changed, False)
    changed = deepcopy(rows)
    del changed[0]['fd']
    check('missing_fd', changed, False)
    changed = deepcopy(rows)
    changed[0], changed[1] = changed[1], changed[0]
    check('fatal_order', changed, False)
    changed = deepcopy(limited)
    changed[-1]['recorded'] = 511
    changed[-1]['omitted'] = 9
    check('wrong_limit_counts', changed, False)
    changed = deepcopy(limited)
    changed[-2]['log_index'] = 513
    check('index_past_limit', changed, False)
    return checks


def original_checks(path, fixtures):
    import probe_stock_init as probe
    rows = probe.read_observations(path)
    layouts = [row for row in rows if row.get('config') == 'translation_guest_layout']
    if len(layouts) != 1:
        raise ValueError('原日志需要一份 translation_guest_layout：' + str(path))
    guest = layouts[0]
    renamed, legacy = [], 0
    for row in rows:
        if 'stock_init_liblog_fatal' in row and 'trace_index' in row:
            if 'log_index' in row:
                raise ValueError('原诊断行同时包含两种序号：' + str(path))
            row = {('log_index' if key == 'trace_index' else key): value for key, value in row.items()}
            legacy += 1
        renamed.append(row)
    ordinary = [row for row in rows if 'stock_init_liblog_fatal' not in row and 'stock_init_liblog_summary' not in row]
    record = {
        'path': str(path), 'sha256': sha(path), 'rows': len(rows),
        'legacy_index_rows_renamed_in_memory': legacy,
        'normalized_liblog_validated': liblog.validate_liblog(renamed, required=True),
        'normalized_syscall_validated': probe._validate_syscall_trace(renamed, guest),
        'ordinary_syscall_validated': probe._validate_syscall_trace(ordinary, guest),
        'mixed_fixtures': [],
    }
    cleanup = next((index for index, row in enumerate(ordinary) if row.get('test') == 'translated_context_cleanup'), len(ordinary))
    for name, fixture in fixtures.items():
        combined = ordinary[:cleanup] + fixture + ordinary[cleanup:]
        record['mixed_fixtures'].append({
            'name': name, 'rows': len(combined),
            'liblog_validated': liblog.validate_liblog(combined, required=True),
            'syscall_validated': probe._validate_syscall_trace(combined, guest),
        })
    record['passed'] = (record['normalized_liblog_validated'] and record['normalized_syscall_validated'] and
                        record['ordinary_syscall_validated'] and
                        all(row['liblog_validated'] and row['syscall_validated'] for row in record['mixed_fixtures']))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--original-log', type=Path, action='append', default=[],
                        help='只读检查实际原 JSONL；保留旧诊断的 trace_index 仅在内存中改为 log_index')
    args = parser.parse_args()
    common = Path(subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = (ROOT / common).resolve() if not common.is_absolute() else common.resolve()
    output = args.output.resolve()
    if not output.is_relative_to(common.parent / 'work'):
        parser.error('输出需要位于 MAIN/work')
    output.mkdir(parents=True, exist_ok=False)
    sources = (Path(__file__).resolve(), Path(liblog.__file__).resolve(), HEADER, FIXTURE)
    before = {str(path): sha(path) for path in sources}
    report = {
        'test': 'stock_init_liblog', 'passed': False, 'host': platform.platform(),
        'source_sha256_before': before, 'fixtures': [], 'validator_checks': [], 'original_logs': [],
        'scope': '宿主 fixture 调用实际四向量 FATAL 采集头；生产协议 validator 与可选原行的 syscall 采样兼容检查。',
        'guest_started': False,
    }
    try:
        compiler = shutil.which('clang')
        if not compiler:
            raise RuntimeError('缺少 clang')
        report['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True, timeout=10).splitlines()[0]
        binary = output / 'fixture'
        build = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(FIXTURE), '-o', str(binary)]
        report['build_command'] = build
        with (output / 'build.log').open('w') as log:
            subprocess.run(build, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        fixtures = {}
        for name in ('protocol', 'boundary', 'limit'):
            stdout_path, stderr_path = output / (name + '.jsonl'), output / (name + '.stderr')
            with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
                subprocess.run([str(binary), name], stdout=stdout, stderr=stderr, check=True, timeout=10)
            if any(path.stat().st_size > FILE_LIMIT for path in (stdout_path, stderr_path)):
                raise ValueError('fixture 输出超过 4 MiB')
            rows = [json.loads(line) for line in stdout_path.read_text().splitlines()]
            metadata = json.loads(stderr_path.read_text())
            checks = fixture_checks(name, rows, metadata)
            report['fixtures'].append({'name': name, 'checks': checks, 'stdout_bytes': stdout_path.stat().st_size,
                                       'passed': all(check['passed'] for check in checks)})
            fixtures[name] = rows
        report['validator_checks'] = validator_checks(fixtures['protocol'], fixtures['limit'])
        for path in args.original_log:
            report['original_logs'].append(original_checks(path.resolve(), fixtures))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    report['source_sha256_after'] = {path: sha(path) for path in before}
    report['source_unchanged'] = before == report['source_sha256_after']
    report['passed'] = ('error' not in report and report['source_unchanged'] and len(report['fixtures']) == 3 and
                        all(row['passed'] for row in report['fixtures']) and
                        all(row['passed'] for row in report['validator_checks']) and
                        all(row['passed'] for row in report['original_logs']))
    report['artifacts'] = {path.name: {'bytes': path.stat().st_size, 'sha256': sha(path)}
                           for path in sorted(output.iterdir()) if path.is_file()}
    (output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'fixtures': len(report['fixtures']),
                      'validator_checks': len(report['validator_checks']), 'original_logs': len(report['original_logs']),
                      'report': str(output / 'result.json')}, ensure_ascii=False))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
