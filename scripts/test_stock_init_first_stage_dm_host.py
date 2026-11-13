#!/usr/bin/env python3
"""用真实 C DM 记录和合成 raw caller 检查宿主回收版本、拒绝与顺序。"""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from exec_aslr import elf_spec_bytes
from probe_stock_init import validate
from stock_init_sdk_ramdisk import read
from test_stock_init_first_stage import DEVICE_FIXTURES, ROOT, check, digest, load_archive, marker, raw_rows


def host_rows(run, ramdisk, spec, dm, page):
    rows, manifest = raw_rows(run, ramdisk, spec, dm, page)
    observation = marker(rows, 'stock_init_observation')
    roles = {'namespace': 1}
    if run['mode'] == 'lifecycle':
        lifecycle = marker(rows, 'stock_init_lifecycle')
        roles = {'namespace': lifecycle['pid'], 'child': lifecycle['child'],
                 'ordinary': marker(rows, 'stock_init_owned_ordinary')['pid']}
    # C fixture 只执行 DM 函数；运行中 namespace 与 guest 行按真实 caller 字段合成。
    running = {'stock_init_first_stage_dm_host_boundary': True, 'operation': 'running_namespace',
               'result': -16, 'ancestor_pid': observation['ancestor_pid']}
    position = rows.index(marker(rows, 'stock_init_ramdisk_ready')) + 1
    rows = rows[:position] + [running] + rows[position:]
    denied = [{'stock_init_first_stage_dm_guest_denied': True, 'pid': pid, 'role': role, 'errno': 38}
              for role, pid in roles.items()]
    cleanups = [row for row in rows if 'stock_init_first_stage_dm_cleanup' in row]
    if cleanups:
        position = rows.index(cleanups[0]) + 1
        rows = rows[:position] + denied + rows[position:]
    return rows, manifest


def changed(rows, event, **fields):
    return [{**row, **fields} if row is event else row for row in rows]


def before(rows, event, target):
    result = list(rows)
    result.remove(event)
    result.insert(result.index(target), event)
    return result


def check_boundaries(cases, rows, accepted, label):
    host = marker(rows, 'stock_init_first_stage_dm_host_cleanup')
    parameters, running = [row for row in rows if 'stock_init_first_stage_dm_host_boundary' in row]
    denied = [row for row in rows if 'stock_init_first_stage_dm_guest_denied' in row]
    prepared = marker(rows, 'stock_init_first_stage_dm_prepare')
    cmdline = marker(rows, 'stock_init_first_stage_cmdline')
    ready = marker(rows, 'stock_init_ramdisk_ready')
    observation = marker(rows, 'stock_init_observation')
    reaped = marker(rows, 'stock_init_host_stop_reaped')
    ordinary = [row for row in rows if row.get('phase') == 'cleanup' and 'command' in row]
    closed = next(row for row in rows if row.get('phase') == 'cleanup' and row.get('operation') == 'close_control')
    cleanup = marker(rows, 'stock_init_first_stage_dm_cleanup')
    stdout = next(row for row in rows if 'stock_init_output' in row and row['fd'] == 1)
    variants = []
    for name, event in [('host', host), ('parameters', parameters), ('running', running), ('guest', denied[0])]:
        variants += [(name + '_missing', [row for row in rows if row is not event]),
                     (name + '_duplicate', rows[:rows.index(event)] + [event] + rows[rows.index(event):]),
                     (name + '_extra_field', changed(rows, event, extra=0))]
        flag = next(key for key in event if key.startswith('stock_init_first_stage_'))
        variants.append((name + '_marker_int', changed(rows, event, **{flag: 1})))
    for name, fields in [('syscall_other', {'syscall': 247}), ('syscall_float', {'syscall': 248.0}),
                         ('result_bool', {'result': False}), ('result_busy', {'result': -16}),
                         ('result_unsupported', {'result': -38}), ('empty_false', {'empty': False}),
                         ('empty_int', {'empty': 1})]:
        variants.append(('host_' + name, changed(rows, host, **fields)))
    variants.append(('host_syscall_missing', [{key: value for key, value in row.items() if key != 'syscall'}
                                             if row is host else row for row in rows]))
    for name, values in [('short', [-22] * 5), ('long', [-22] * 7), ('error_other', [-22] * 5 + [-13]),
                         ('float', [-22] * 5 + [-22.0]), ('missing', None)]:
        variants.append(('parameters_results_' + name, changed(rows, parameters, results=values)))
    variants.append(('parameters_operation_other', changed(rows, parameters, operation='other')))
    for name, fields in [('result_success', {'result': 0}), ('result_other', {'result': -13}),
                         ('result_float', {'result': -16.0}), ('pid_bool', {'ancestor_pid': True}),
                         ('pid_other', {'ancestor_pid': running['ancestor_pid'] + 1}),
                         ('operation_other', {'operation': 'other'})]:
        variants.append(('running_' + name, changed(rows, running, **fields)))
    for event in denied:
        for name, fields in [('pid_other', {'pid': event['pid'] + 1}), ('pid_bool', {'pid': True}),
                             ('pid_zero', {'pid': 0}), ('pid_limit', {'pid': 1 << 31}),
                             ('errno_other', {'errno': 1}), ('errno_float', {'errno': 38.0}),
                             ('role_other', {'role': 'other'}), ('result_present', {'result': -38})]:
            variants.append(('guest_' + event['role'] + '_' + name, changed(rows, event, **fields)))
    if len(denied) > 1:
        variants.append(('guest_role_duplicate', changed(rows, denied[-1], role=denied[0]['role'], pid=denied[0]['pid'])))
        owned = marker(rows, 'stock_init_owned_ordinary')
        ordinary_ready = marker(rows, 'stock_init_ordinary_ready')
        variants += [('ordinary_ready_pid_other', changed(rows, ordinary_ready, pid=ordinary_ready['pid'] + 1)),
                     ('ordinary_saved_ready_pid_other', changed(rows, owned, ready_pid=owned['pid'] + 1))]
    variants += [('parameters_before_prepare', before(rows, parameters, prepared)),
                 ('parameters_after_cmdline', before(rows, cmdline, parameters)),
                 ('running_before_ready', before(rows, running, ready)),
                 ('running_after_observation', before(rows, running, reaped)),
                 ('guest_before_cleanup', before(rows, denied[0], cleanup)),
                 ('guest_after_stdout_output', before(rows, denied[0],
                                                      rows[rows.index(stdout) + 1])),
                 ('host_before_remove', before(rows, host, ordinary[0])),
                 ('host_before_list', before(rows, host, ordinary[1])),
                 ('host_after_close', before(rows, closed, host)),
                 ('control_close_after_cleanup', before(rows, cleanup, closed)),
                 ('sync_before_reaped', before(rows, marker(rows, 'stock_init_first_stage_sync'),
                                              marker(rows, 'stock_init_host_stop_reaped')))]
    variants += [('stdout_missing', [row for row in rows if row is not stdout]),
                 ('stdout_fd_missing', [{key: value for key, value in row.items() if key != 'fd'}
                                        if row is stdout else row for row in rows]),
                 ('stdout_fd_other', changed(rows, stdout, fd=2)),
                 ('stdout_fd_bool', changed(rows, stdout, fd=True))]
    for name, altered in variants:
        check(cases, 'host_boundary/' + name + '/' + label, lambda altered=altered: accepted(altered), False,
              provenance='synthetic_boundary_of_actual_c_dm_records_and_raw_caller')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--ramdisk', type=Path, required=True)
    parser.add_argument('--legacy-dm-results', type=Path, required=True)
    parser.add_argument('--host-dm-results', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    archive, archive_identity = load_archive(args.archive)
    ramdisk = read(args.ramdisk)
    spec = elf_spec_bytes(next(row for row in ramdisk['entries'] if row['path'] == 'init')['data'], '/init')
    legacy = json.loads(args.legacy_dm_results.read_text())
    fixture = json.loads(args.host_dm_results.read_text())
    dm_cases = {case['case']: case['rows'] for case in fixture['cases']}
    legacy_empty = next(case['rows'] for case in legacy['cases'] if case['case'] == 'empty')
    source_paths = [Path(__file__), *(ROOT / 'scripts' / name for name in
                    ('stock_init_first_stage.py', 'test_stock_init_first_stage.py', 'probe_stock_init.py',
                     'stock_init_sdk_ramdisk.py', 'stock_init_stop.py', 'exec_aslr.py',
                     'stock_init_first_stage_devices.py', 'test_stock_init_first_stage_exec.py'))]
    source_paths += [Path(path) for path in fixture['inputs']]
    source_paths.append(DEVICE_FIXTURES / 'raw-baseline.json')
    source_sha256 = {str(path): digest(path) for path in source_paths}
    cases = []
    check(cases, 'fixture/current_source_identity', lambda: fixture['passed'] is True and
          fixture['source_unchanged'] is True and all(case['validated'] is True for case in fixture['cases']) and
          all(digest(Path(path)) == value for path, value in fixture['inputs'].items()), True,
          provenance='actual_c_runner_fixture_with_registered_source_sha256')
    check(cases, 'fixture/legacy_validated', lambda: legacy['validated'] is True and
          all(case['validated'] is True for case in legacy['cases']), True,
          provenance='historical_c_fixture_with_original_source_identity')
    for report_name in ('init_4k', 'init_16k'):
        report = archive['reports'][report_name]['data']
        page = report['source_configuration']['guest_page_bytes']
        for run in report['runs']:
            inputs = report['input']
            label = report_name + '/' + run['entry'] + '/' + run['phase']
            kwargs = {'guest_page_bytes': page, 'kernel_log': run['stderr'], 'original_input': inputs,
                      'exec_elf_images': inputs['exec_elf_images']}
            check(cases, 'legacy/' + label, lambda: validate(run['results'], run['mode'], True, **kwargs), True,
                  provenance='actual_archived_run')
            if run['phase'] == 'recovery':
                denied = {'stock_init_first_stage_dm_guest_denied': True, 'pid': 1, 'role': 'namespace', 'errno': 38}
                check(cases, 'mounted_guest_diagnostic/' + label,
                      lambda: validate([*run['results'][:-1], denied, run['results'][-1]], run['mode'], True, **kwargs), True,
                      provenance='actual_mounted_recovery_with_synthetic_new_guest_diagnostic')
                continue
            old_rows, manifest = raw_rows(run, ramdisk, spec, legacy_empty, page)
            raw_input = {**inputs, 'first_stage_contract': 1, 'sdk_ramdisk': {'entries': manifest},
                         'exec_elf_images': {'/init': spec, **inputs['exec_elf_images']}}
            host_input = {**raw_input, 'host_dm_cleanup_contract': 1}

            def accepted(rows, selected=host_input):
                return validate(rows, run['mode'], True, guest_page_bytes=page, kernel_log=run['stderr'],
                                original_input=selected, exec_elf_images=selected['exec_elf_images'], first_stage=True)

            check(cases, 'legacy_raw_missing_contract/' + label, lambda: accepted(old_rows, raw_input), True,
                  provenance='synthetic_raw_with_historical_c_dm_records')
            check(cases, 'new_contract_requires_records/' + label, lambda: accepted(old_rows), False)
            for name in ('empty', 'both-ioctls-rejected'):
                rows, _ = host_rows(run, ramdisk, spec, dm_cases[name], page)
                check(cases, 'host_actual/' + name + '/' + label, lambda rows=rows: accepted(rows), True,
                      provenance='actual_c_dm_records_with_synthetic_running_namespace_and_guest_caller')
            if report_name != 'init_16k' or run['entry'] != 'cli':
                continue
            rows, _ = host_rows(run, ramdisk, spec, dm_cases['empty'], page)
            if run['mode'] == 'original':
                check_boundaries(cases, rows, accepted, label)
                extra = {'stock_init_first_stage_dm_guest_denied': True, 'pid': 2, 'role': 'child', 'errno': 38}
                check(cases, 'original_guest_extra_role/' + label, lambda: accepted([*rows, extra]), False)
                executed = marker(rows, 'stock_init_exec')
                cleanup = marker(rows, 'stock_init_first_stage_dm_cleanup')
                stdout = next(row for row in rows if 'stock_init_output' in row and row['fd'] == 1)
                trace = [row for row in rows if 'stock_init_exec_layout' in row or
                         (row.get('stock_init_security') == 221 and row.get('result') == 0)]
                late_trace = [row for row in rows if not any(row is event for event in trace)]
                position = late_trace.index(marker(late_trace, 'stock_init_host_stop_reaped'))
                late_trace[position:position] = trace
                variants = [('exec_marker_before_cleanup', before(rows, executed, cleanup)),
                            ('exec_marker_after_stdout_output', before(rows, executed, rows[rows.index(stdout) + 1])),
                            ('translated_exec_records_after_observation', late_trace)]
                for name, altered in variants:
                    check(cases, name + '/' + label, lambda altered=altered: accepted(altered), False,
                          provenance='synthetic_stdout_or_translated_exec_order_boundary')
                continue
            for name, dm in dm_cases.items():
                if name in ('empty', 'both-ioctls-rejected'):
                    continue
                altered, _ = host_rows(run, ramdisk, spec, dm, page)
                check(cases, 'host_actual_rejected/' + name, lambda altered=altered: accepted(altered), False,
                      provenance='actual_c_dm_failure_records_with_synthetic_raw_caller')
            for value in (None, True, False, 0, -1, 2, 1.0, '1'):
                check(cases, 'contract_type/' + repr(value),
                      lambda value=value: accepted(rows, {**raw_input, 'host_dm_cleanup_contract': value}), False)
            for kind in ('stock_init_first_stage_dm_host_cleanup', 'stock_init_first_stage_dm_host_boundary',
                         'stock_init_first_stage_dm_guest_denied'):
                event = marker(rows, kind)
                altered = old_rows[:1] + [event] + old_rows[1:]
                check(cases, 'missing_contract_new_marker/' + kind, lambda altered=altered: accepted(altered, raw_input), False)
            check(cases, 'missing_contract_new_report', lambda: accepted(rows, raw_input), False)
            check_boundaries(cases, rows, accepted, label)
            denied_rows, _ = host_rows(run, ramdisk, spec, dm_cases['both-ioctls-rejected'], page)
            ordinary = [row for row in denied_rows if row.get('phase') == 'cleanup' and 'command' in row]
            for event in ordinary:
                for name, fields in [('version_major', {'version': [5, 48, 0]}),
                                     ('version_minor', {'version': [4, 49, 0]}),
                                     ('version_patch', {'version': [4, 48, 1]}),
                                     ('size', {'data_size': 312}), ('start', {'data_start': 320}),
                                     ('flags', {'flags': 1}), ('dev', {'dev': 1}), ('first_dev', {'first_dev': 1}),
                                     ('observed', {'first_dev_observed': not event['first_dev_observed']}),
                                     ('within', {'first_dev_within_data_size': True})]:
                    altered = changed(denied_rows, event, **fields)
                    check(cases, 'denied_header/' + str(event['command']) + '/' + name,
                          lambda altered=altered: accepted(altered), False,
                          provenance='synthetic_boundary_of_preserved_eacces_request_header')
            newer = [{**row, 'version': [4, 49, 1]} if 'stock_init_first_stage_dm_ioctl' in row else row for row in rows]
            check(cases, 'success_header_existing_version_range', lambda: accepted(newer), True)
    result = {'test': 'stock_init_first_stage_dm_host_contract', 'passed': all(case['passed'] for case in cases),
              'cases': cases, 'source_sha256': source_sha256,
              'source_unchanged': all(digest(Path(path)) == value for path, value in source_sha256.items()),
              'inputs': {'archive': archive_identity,
                         'legacy_dm_fixture': {'path': str(args.legacy_dm_results.resolve()), 'sha256': digest(args.legacy_dm_results)},
                         'host_dm_fixture': {'path': str(args.host_dm_results.resolve()), 'sha256': digest(args.host_dm_results)},
                         'ramdisk': {key: ramdisk[key] for key in ('input', 'input_sha256', 'decoded_sha256')}},
              'guest_executed': False, 'candidate_disks_opened': False,
              'scope': '两页旧真实入口回放；DM prepare、参数拒绝和cleanup行来自真实C宿主fixture；raw部署、running_namespace与guest拒绝行为合成，完整first-stage与boot未实跑。'}
    result['passed'] &= result['source_unchanged']
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': len(cases),
                      'failed': [case['case'] for case in cases if not case['passed']]}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
