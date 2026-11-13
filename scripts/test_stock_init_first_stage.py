#!/usr/bin/env python3
"""回放两页旧归档，并检查明确标注合成的 raw first-stage 验收边界。"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import posixpath
import stat
import subprocess
import sys

sys.dont_write_bytecode = True
from exec_aslr import elf_spec_bytes
from probe_stock_init import validate
from stock_init_first_stage import progress, require_complete
from stock_init_first_stage_devices import observe as observe_devices, validate as validate_devices
from stock_init_sdk_ramdisk import read
from test_stock_init_first_stage_exec import exec_rows, raw_fixture

ROOT = Path(__file__).resolve().parents[1]
DEVICE_FIXTURES = ROOT / 'work/first-stage-device-source-review-v1/host-fixture-v2'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load_archive(path):
    packed = path.read_bytes()
    decoded = (subprocess.run(['zstd', '-d', '-q', '-c', '-'], input=packed,
                              capture_output=True, check=True, timeout=30).stdout
               if path.suffix == '.zst' else packed)
    return json.loads(decoded), {'path': str(path.resolve()), 'sha256': hashlib.sha256(packed).hexdigest(),
                                'decoded_sha256': hashlib.sha256(decoded).hexdigest()}


def marker(rows, name):
    return next(row for row in rows if name in row)


def replace(rows, name, **fields):
    old = marker(rows, name)
    return [{**row, **fields} if row is old else row for row in rows]


def move_before(rows, name, before):
    result = list(rows)
    row = marker(result, name)
    result.remove(row)
    result.insert(result.index(marker(result, before)), row)
    return result


def device_rows(ranchu, records=None):
    if records is None:
        path = DEVICE_FIXTURES / ('raw-baseline.json' if ranchu else 'default-baseline.json')
        records = json.loads(path.read_text())['results']
    # mount/unmount 是合成 caller 行；查询值和返回直接读取实际 C fixture。
    mounts = [{'stock_init_first_stage_sysfs_mount': True, 'operation': operation, 'result': 0}
              for operation in (('mount', 'unmount') if ranchu else ('mount',))]
    return [mounts[0], *records, *mounts[1:]]


def sysfs_query(rows, path):
    return next(row for row in rows if row.get('stock_init_first_stage_sysfs') is True and
                row['path_hex'] == path.encode().hex())


def sysfs_content(rows, path, value):
    original = sysfs_query(rows, path)
    fields = {'value_hex': value.hex(), 'result': 0}
    fields.update(read_results=[len(value), 0] if original['operation'] == 'read' else None,
                  readlink_results=[len(value)] if original['operation'] == 'readlink' else None)
    return [{**row, **fields} if row is original else row for row in rows]


def raw_rows(run, ramdisk, spec, dm, page):
    original = run['results']
    manifest = [{key: value for key, value in entry.items() if key != 'data'} for entry in ramdisk['entries']]
    deployed = []
    for index, entry in enumerate(manifest, 1):
        deployed.append({'stock_init_ramdisk_entry': index, 'archive': entry['archive'], 'path': '/' + entry['path'],
                         'mode': entry['mode'], 'uid': entry['uid'], 'gid': entry['gid'], 'nlink': entry['nlink'],
                         'size': entry['bytes'], 'mtime_sec': entry['mtime'], 'mtime_nsec': 0,
                         'rdev_major': entry['rdev_major'], 'rdev_minor': entry['rdev_minor'], 'content_matches': True})
    # 目录 nlink/size 由 Linux 创建；本合成检查只使用验收需要的非零 nlink。
    for entry in deployed:
        if stat.S_IFMT(entry['mode']) == stat.S_IFDIR:
            entry.update(nlink=2, size=0)
    prepare = [row for row in dm if row.get('phase') == 'prepare' or 'stock_init_first_stage_dm_prepare' in row]
    finish = [row for row in dm if row.get('phase') == 'cleanup' or 'stock_init_first_stage_dm_cleanup' in row]
    rows = [*prepare,
            {'stock_init_first_stage_cmdline': True, 'value_hex': b'console=ttyS0 androidboot.hardware=ranchu\n'.hex()},
            *deployed, *device_rows(True), {'stock_init_ramdisk_ready': True, 'entries': 29, 'proc_restored': True},
            next(row for row in original if row.get('config') == 'translation_guest_layout'),
            *[row for row in original if row.get('test') == 'linux_signal_kernel_restorer']]
    if run['mode'] == 'original':
        generated = raw_fixture(exec_rows(original), spec, page)
        rows += [{'stock_init_exec': True, 'pid': 1, 'path': '/init', 'arguments': []}]
        rows += [row for row in generated if 'stock_init_exec_layout' in row or 'stock_init_security' in row]
        contexts = next(row for row in generated if row.get('test') == 'translated_context_cleanup')
    else:
        rows.append(marker(original, 'stock_init_lifecycle'))
        contexts = next(row for row in original if row.get('test') == 'translated_context_cleanup')
    kinds = ('stock_init_host_stop_boundaries', 'stock_init_ordinary_ready', 'stock_init_owned_ordinary',
             'stock_init_stop_request', 'stock_init_host_stop', 'stock_init_observation', 'stock_init_host_stop_reaped')
    rows += [row for row in original if any(kind in row for kind in kinds)]
    rows += [{'stock_init_first_stage_sync': True, 'result': 0}, *finish,
             {'stock_init_output': True, 'fd': 1, 'bytes': 0, 'read_result': 0},
             {'stock_init_output': True, 'fd': 2, 'bytes': 0, 'read_result': 0}, marker(original, 'stock_init_kernel_log')]
    rows += [row for row in original if 'host_disk_close' in row]
    rows += [contexts, marker(original, 'stock_init_shutdown'), {'result': 'passed'}]
    return rows, manifest


def check_ramdisk_endings(cases, raw, accepted, label):
    devices = [row for row in raw if 'stock_init_ramdisk_entry' in row and stat.S_IFMT(row['mode']) == stat.S_IFCHR]
    diagnostics = [{'stock_init_ramdisk_mknod': row['stock_init_ramdisk_entry'], 'path': row['path'],
                    'rdev_major': row['rdev_major'], 'rdev_minor': row['rdev_minor'], 'result': 0}
                   for row in devices]
    offset = raw.index(marker(raw, 'stock_init_ramdisk_entry'))
    created = raw[:offset] + diagnostics + raw[offset:]
    check(cases, 'ramdisk_endings/new_devices/' + label, lambda: accepted(created), True,
          provenance='synthetic_mknod_diagnostics_from_original_ramdisk_entries')
    console = next(row for row in devices if row['path'] == '/dev/console')
    existing = [{**row, 'result': -17} if row['path'] == console['path'] else row for row in diagnostics]
    console_index = next(index for index, row in enumerate(existing) if row['path'] == console['path'])
    existing.insert(console_index + 1, {'stock_init_ramdisk_existing_device': console['stock_init_ramdisk_entry'],
                    'path': console['path'], 'statx_result': 0, 'mask': 0x7ff,
                    'mode': stat.S_IFCHR | 0o600, 'nlink': console['nlink'],
                    'rdev_major': console['rdev_major'], 'rdev_minor': console['rdev_minor']})
    reused = raw[:offset] + existing + raw[offset:]
    check(cases, 'ramdisk_endings/existing_console/' + label, lambda: accepted(reused), True,
          provenance='synthetic_eexist_and_statx_diagnostics_with_original_ramdisk_device')
    variants = [
        ('final_missing', [row for row in reused if row != {'result': 'passed'}]),
        ('final_duplicate', [*reused, {'result': 'passed'}]),
        ('final_failed', [{'result': 'failed'} if row == {'result': 'passed'} else row for row in reused]),
        ('unknown_result_extra', [*reused, {'result': 0}]),
    ]
    for name, rows in variants:
        check(cases, 'ramdisk_endings/' + name + '/' + label, lambda rows=rows: accepted(rows), False,
              provenance='synthetic_final_result_boundary_with_device_diagnostics')


def progress_rows(inputs, dynamic_pair, partition_order, numeric=False):
    def block(number, paths, **arguments):
        args = [0] * 6
        for position, value in arguments.items():
            args[int(position)] = value
        return {'stock_init_block': number, 'pid': 1, 'result': 0, 'args': args,
                'paths_hex': [path.encode().hex() for path in paths]}

    platform = '/dev/block/platform/a003c00.virtio_mmio/by-name/'
    observations = device_rows(True)
    devices = observe_devices(observations, inputs, True)
    # 节点和 numeric backing 使用 C fixture 解析的设备号；DM 节点仍为合成。
    rows = [*observations,
            block(33, ['/dev/block/ncedisk1p2'], **{'0': (1 << 64) - 100, '2': 0o60600,
                                              '3': devices['ncedisk1p2']['device']}),
            block(36, ['/dev/block/ncedisk1p2', platform + 'super']),
            block(36, ['/dev/block/ncedisk1p2', '/dev/block/by-name/super']),
            block(33, ['/dev/block/ncedisk2p1'], **{'0': (1 << 64) - 100, '2': 0o60600,
                                              '3': devices['ncedisk2p1']['device']}),
            block(36, ['/dev/block/ncedisk2p1', platform + 'metadata']),
            block(36, ['/dev/block/ncedisk2p1', '/dev/block/by-name/metadata']),
            block(40, [platform + 'metadata', '/metadata', 'ext4'])]
    mapped = {}
    for index, name in enumerate(partition_order):
        expected = inputs['sdk_partitions'][name]
        target = {'sector_start': 0, 'length': expected['sizelimit'] // 512, 'type_hex': b'linear'.hex(),
                  'backing_start_sector': (expected['offset'] - inputs['lp_metadata']['super_offset']) // 512}
        target.update({'backing_major': devices['ncedisk1p2']['major'], 'backing_minor': devices['ncedisk1p2']['minor']}
                      if numeric else {'backing_path_hex': b'/dev/block/ncedisk1p2'.hex()})
        rows += [{'stock_init_dm_table_load': True, 'pid': 1, 'fd': 10, 'result': 0, 'read_result': 0,
                  'version': [4, 48, 0], 'data_size': 400, 'data_start': 312, 'target_count': 1,
                  'flags': 1, 'dev': 0, 'name_hex': name.encode().hex(), 'targets': [target]},
                 block(33, ['/dev/block/dm-' + str(index)], **{'0': (1 << 64) - 100, '2': 0o60600, '3': (253 << 8) | index}),
                 block(36, ['/dev/block/dm-' + str(index), '/dev/block/mapper/' + name])]
        mapped[name] = '/dev/block/dm-' + str(index)
    rows += [block(40, [mapped['system'], '/system', inputs['sdk_partitions']['system']['filesystem']], **{'3': 1}),
             block(49, ['/system']), block(40, ['/system', '/', ''], **{'3': 8192}), block(51, ['.'])]
    for name in partition_order:
        if name != 'system':
            rows.append(block(40, [mapped[name], '/' + name, inputs['sdk_partitions'][name]['filesystem']], **{'3': 1}))
    rows += [block(35, ['/init']), *dynamic_pair]
    return rows


def check(cases, name, operation, expected, **context):
    try:
        actual = operation()
        cases.append({'case': name, 'expected': expected, 'observed': actual,
                      'passed': actual is expected, **context})
    except Exception as error:
        cases.append({'case': name, 'expected': expected, 'passed': False,
                      'error_type': type(error).__name__, 'error': str(error), **context})


def check_completion(cases, archived, first_stage):
    baseline = {'runs': [{**run, 'first_stage': copy.deepcopy(first_stage)} if run['mode'] == 'original' else run
                         for run in archived['runs']], 'first_stage_completed': True}

    def accepted(report, lifecycle_only=False):
        try:
            return require_complete(report, lifecycle_only) is None
        except ValueError:
            return False

    check(cases, 'completion/cli_and_gcd_completed', lambda: accepted(baseline), True,
          provenance='actual_archived_runs_with_synthetic_completed_progress')
    lifecycle = {'runs': [run for run in archived['runs'] if run['mode'] == 'lifecycle'],
                 'first_stage_completed': False}
    check(cases, 'completion/lifecycle_only', lambda: accepted(lifecycle, True), True)
    check(cases, 'completion/lifecycle_without_original', lambda: accepted(lifecycle), False)
    check(cases, 'completion/summary_incomplete', lambda: accepted({**baseline, 'first_stage_completed': False}), False)
    check(cases, 'completion/summary_requires_boolean_true',
          lambda: accepted({**baseline, 'first_stage_completed': 1}), False)
    original = next(index for index, run in enumerate(baseline['runs'])
                    if run['mode'] == 'original' and run['entry'] == 'gcd')
    changes = [('run_incomplete', {'first_stage': {**first_stage, 'completed': False}}),
               ('observation_failed', {'observation_validated': False}),
               ('duplicate_cli', {'entry': 'cli'}),
               ('mounted_recovery_phase', {'phase': 'recovery'}),
               ('missing_progress', {'first_stage': None})]
    for name, fields in changes:
        changed = {**baseline, 'runs': [*baseline['runs']]}
        changed['runs'][original] = {**changed['runs'][original], **fields}
        check(cases, 'completion/' + name, lambda changed=changed: accepted(changed), False)
    missing = {**baseline, 'runs': [run for index, run in enumerate(baseline['runs']) if index != original]}
    extra = {**baseline, 'runs': [*baseline['runs'], baseline['runs'][original]]}
    check(cases, 'completion/missing_gcd', lambda: accepted(missing), False)
    check(cases, 'completion/extra_original', lambda: accepted(extra), False)
    check(cases, 'completion/runs_not_array', lambda: accepted({**baseline, 'runs': {}}), False)
    check(cases, 'completion/run_not_object', lambda: accepted({**baseline, 'runs': [*baseline['runs'], None]}), False)


def check_table_schema(cases, raw, accepted, trace):
    tables = {case['case']: case['rows'][0] for case in trace['dm'] if case['rows']}
    position = raw.index(marker(raw, 'stock_init_stop_request'))

    def accepted_table(row):
        return accepted(raw[:position] + [row] + raw[position:])

    for name, row in tables.items():
        check(cases, 'dm_schema/actual/' + name, lambda row=row: accepted_table(row), True,
              provenance='actual_c_trace_rows_in_synthetic_raw', read_result=row['read_result'])
    numeric, path, nonlinear = (tables[name] for name in ('numeric', 'path', 'nonlinear'))
    variants = [('success_version_other', {**numeric, 'version': [5, 48, 0]}, False),
                ('success_start_below_header', {**numeric, 'data_start': 304}, False),
                ('success_start_unaligned', {**numeric, 'data_start': 316}, False),
                ('success_span_exact_target', {**numeric, 'data_size': 352}, False),
                ('success_span_outside_size', {**numeric, 'data_size': 311}, False),
                ('success_target_missing', {**numeric, 'targets': []}, False),
                ('success_target_count_two', {**numeric, 'target_count': 2}, False),
                ('success_name_unterminated', {**numeric, 'name_hex': tables['name_unterminated']['name_hex']}, False),
                ('success_linear_backing_missing', {**numeric, 'targets': [
                    {key: numeric['targets'][0][key] for key in ('sector_start', 'length', 'type_hex')}]}, False),
                ('failure_backing_present', {**numeric, 'read_result': -14}, False),
                ('failure_unknown_read_result', {**numeric, 'read_result': -5, 'targets': []}, False)]
    limit_target = {**nonlinear['targets'][0], 'type_hex': (b'x' * 15).hex()}
    limit = {**nonlinear, 'name_hex': (b'n' * 127).hex(), 'data_start': 320,
             'data_size': 361, 'targets': [limit_target]}
    variants += [('success_header_limits', limit, True),
                 ('success_type_unterminated', {**limit, 'targets': [{**limit_target, 'type_hex': (b'x' * 16).hex()}]}, False),
                 ('success_nonlinear_backing_present', {**numeric, 'targets': [
                     {**numeric['targets'][0], 'type_hex': b'crypt'.hex()}]}, False)]
    for name, value, expected in (
            ('path_minimum', b'/dev/block/a', True), ('path_maximum', b'/dev/block/' + b'a' * 244, True),
            ('path_too_long', b'/dev/block/' + b'a' * 245, False),
            ('path_relative', b'dev/block/vdb1', False), ('path_outside', b'/dev/ncesystem', False),
            ('path_prefix_collision', b'/dev/blockade/vdb1', False),
            ('path_empty_component', b'/dev/block//vdb1', False), ('path_dot', b'/dev/block/./vdb1', False),
            ('path_dotdot', b'/dev/block/../vdb1', False), ('path_trailing_slash', b'/dev/block/vdb1/', False),
            ('path_nul', b'/dev/block/vdb1\0', False), ('path_space', b'/dev/block/vdb 1', False)):
        variants.append((name, {**path, 'targets': [{**path['targets'][0], 'backing_path_hex': value.hex()}]}, expected))
    for name, row, expected in variants:
        check(cases, 'dm_schema/synthetic/' + name, lambda row=row: accepted_table(row), expected,
              provenance='synthetic_boundary_of_c_before_sampling')


def check_sysfs_contract(cases, inputs, raw, accepted, fixture):
    success_faults = {'none', 'open-eintr', 'open-many-eintr', 'read-eintr', 'read-short', 'link-eintr'}
    for case in fixture['cases']:
        flags = case['flags']
        selected = dict(inputs)
        for present, key in zip(flags[1:], ('data_image', 'metadata_image')):
            if present != 1:
                selected.pop(key, None)
        ranchu = flags[0] == 1
        observed = device_rows(ranchu, case['results'])
        expected = all(flag in (0, 1) for flag in flags) and case['fault'] in success_faults
        check(cases, 'sysfs/actual/' + str(case['case']) + '/' + case['fault'],
              lambda observed=observed, selected=selected, ranchu=ranchu: validate_devices(observed, selected, ranchu),
              expected, provenance='actual_c_rows_with_synthetic_host_values', flags=flags)
    base = '/sys/class/block/'
    first = marker(raw, 'stock_init_first_stage_sysfs')
    read_row = sysfs_query(raw, base + 'vdc/dev')
    summary = marker(raw, 'stock_init_first_stage_devices')
    missing_field = [{key: value for key, value in row.items() if key != 'close_result'} if row is read_row else row for row in raw]
    changed = lambda old, **fields: [{**row, **fields} if row is old else row for row in raw]
    variants = [
        ('query_missing', [row for row in raw if row is not read_row]),
        ('query_duplicate', raw[:raw.index(read_row)] + [read_row] + raw[raw.index(read_row):]),
        ('summary_duplicate', raw[:raw.index(summary)] + [summary] + raw[raw.index(summary):]),
        ('query_field_missing', missing_field), ('query_field_extra', changed(read_row, extra=True)),
        ('path_hex_invalid', changed(first, path_hex='zz')), ('value_hex_invalid', changed(read_row, value_hex='zz')),
        ('summary_flag_bool', changed(summary, ranchu=True)),
        ('close_eintr_with_success', changed(read_row, close_result=-4)),
        ('close_bool', changed(read_row, close_result=False)),
        ('open_invalid_retry', changed(read_row, open_results=[-5, read_row['open_results'][-1]])),
        ('read_missing_eof', changed(read_row, read_results=[len(bytes.fromhex(read_row['value_hex']))])),
        ('read_total_short', changed(read_row, read_results=[len(bytes.fromhex(read_row['value_hex'])) - 1, 0])),
        ('read_invalid_retry', changed(read_row, read_results=[-5, len(bytes.fromhex(read_row['value_hex'])), 0])),
        ('readlink_count_wrong', changed(first, readlink_results=[len(bytes.fromhex(first['value_hex'])) - 1])),
        ('absent_link_unexpected', sysfs_content(raw, base + 'ncedisk0', b'../../devices/virtual/block/ncedisk0')),
        ('mount_before_deployed', move_before(raw, 'stock_init_first_stage_sysfs_mount', 'stock_init_ramdisk_entry')),
        ('ready_before_unmount', move_before(raw, 'stock_init_ramdisk_ready', 'stock_init_first_stage_devices')),
        ('summary_before_queries', move_before(raw, 'stock_init_first_stage_devices', 'stock_init_first_stage_sysfs')),
    ]
    for name, path, value in (
            ('whole_size_wrong', 'ncedisk1/size', b'8624129\n'),
            ('whole_readonly_wrong', 'ncedisk1/ro', b'0\n'), ('partition_readonly_wrong', 'ncedisk1p2/ro', b'0\n'),
            ('partition_start_wrong', 'ncedisk1p2/start', b'4097\n'),
            ('partition_length_wrong', 'ncedisk1p2/size', b'8617985\n'),
            ('decimal_leading_zero', 'ncedisk1p2/partition', b'02\n'),
            ('device_major_overflow', 'ncedisk1p2/dev', b'4096:14\n'),
            ('device_minor_overflow', 'ncedisk1p2/dev', b'259:1048576\n'),
            ('value_non_ascii', 'vdc/dev', b'254:0\xff\n')):
        variants.append((name, sysfs_content(raw, base + path, value)))
    event_path = base + 'ncedisk1p2/uevent'
    uevent = bytes.fromhex(sysfs_query(raw, event_path)['value_hex'])
    for name, value in (
            ('uevent_name_wrong', uevent.replace(b'DEVNAME=ncedisk1p2', b'DEVNAME=other')),
            ('uevent_partname_wrong', uevent.replace(b'PARTNAME=super', b'PARTNAME=vbmeta')),
            ('uevent_duplicate', uevent + b'MAJOR=259\n'), ('uevent_unterminated', uevent[:-1]),
            ('uevent_diskseq_wrong', uevent.replace(b'DISKSEQ=71', b'DISKSEQ=70'))):
        variants.append((name, sysfs_content(raw, event_path, value)))
    device_path = base + 'ncedisk1/device'
    platform = '/sys/devices/platform/a003c00.virtio_mmio'
    wrong_relative = posixpath.relpath(platform, posixpath.dirname(device_path)).encode()
    variants.append(('relative_parent_from_class_directory', sysfs_content(raw, device_path, wrong_relative)))
    devices = observe_devices(raw, inputs, True)
    source, target = devices['ncedisk1p2'], devices['ncedisk2p1']
    duplicate = sysfs_content(raw, base + 'ncedisk2p1/dev', f"{source['major']}:{source['minor']}\n".encode())
    path = base + 'ncedisk2p1/uevent'
    event = bytes.fromhex(sysfs_query(raw, path)['value_hex'])
    duplicate = sysfs_content(duplicate, path, event.replace(
        f"MINOR={target['minor']}\n".encode(), f"MINOR={source['minor']}\n".encode()))
    variants.append(('partition_device_number_duplicate', duplicate))
    for name, changed_rows in variants:
        check(cases, 'sysfs/synthetic/' + name, lambda changed_rows=changed_rows: accepted(changed_rows), False,
              provenance='synthetic_boundary_of_actual_c_records')
    for ranchu in (False, True):
        observed = device_rows(ranchu)
        path = base + 'ncedisk1p2/subsystem'
        wrong = posixpath.relpath('/sys/class/block', posixpath.dirname(path)).encode()
        check(cases, 'sysfs/relative_partition_from_class/' + str(int(ranchu)),
              lambda observed=observed, wrong=wrong, ranchu=ranchu: validate_devices(
                  sysfs_content(observed, path, wrong), inputs, ranchu), False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--ramdisk', type=Path, required=True)
    parser.add_argument('--dm-results', type=Path, required=True)
    parser.add_argument('--trace-results', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    archive, identity = load_archive(args.archive)
    ramdisk = read(args.ramdisk)
    init = next(entry for entry in ramdisk['entries'] if entry['path'] == 'init')
    spec = elf_spec_bytes(init['data'], '/init')
    dm_results = json.loads(args.dm_results.read_text())
    dm_cases = {case['case']: case['rows'] for case in dm_results['cases']}
    trace_results = json.loads(args.trace_results.read_text())
    device_fixture = json.loads((DEVICE_FIXTURES / 'result.json').read_text())
    cases = []
    source_paths = [Path(__file__), *(ROOT / 'scripts' / name for name in
                    ('stock_init_first_stage.py', 'probe_stock_init.py', 'stock_init_sdk_ramdisk.py',
                     'stock_init_stop.py', 'exec_aslr.py', 'test_stock_init_first_stage_exec.py',
                     'stock_init_first_stage_devices.py'))]
    source_paths += [ROOT / 'experiments/native-linux/translation/stock-init-first-stage-devices.h',
                     *(DEVICE_FIXTURES / name for name in ('raw-baseline.json', 'default-baseline.json', 'result.json'))]
    initial_sha = {str(path.relative_to(ROOT)): digest(path) for path in source_paths}
    trace_sources = [ROOT / 'experiments/native-linux/translation' / name
                     for name in ('stock-init-trace.h', 'stock-init-dm-abi.h')]
    initial_sha.update({str(path.relative_to(ROOT)): digest(path) for path in trace_sources})
    check(cases, 'dm_schema/fixture_source_matches', lambda: trace_results['passed'] is True and all(
        trace_results['inputs'].get(str(path)) == digest(path) for path in trace_sources), True)
    check(cases, 'sysfs/fixture_source_matches', lambda: device_fixture['passed'] is True and
          device_fixture['inputs']['header']['sha256'] == digest(
              ROOT / 'experiments/native-linux/translation/stock-init-first-stage-devices.h'), True)
    for ranchu, name in ((1, 'raw-baseline.json'), (0, 'default-baseline.json')):
        baseline = json.loads((DEVICE_FIXTURES / name).read_text())
        actual = next(case for case in device_fixture['cases'] if case['flags'] == [ranchu, 1, 1] and case['fault'] == 'none')
        check(cases, 'sysfs/baseline_matches_actual/' + name,
              lambda baseline=baseline, actual=actual: baseline['results'] == actual['results'], True)
    for report_name in ('init_4k', 'init_16k'):
        report = archive['reports'][report_name]['data']
        inputs = report['input']
        page = report['source_configuration']['guest_page_bytes']
        for run in report['runs']:
            label = report_name + '/' + run['entry'] + '/' + run['phase']
            kwargs = {'guest_page_bytes': page, 'kernel_log': run['stderr'], 'original_input': inputs,
                      'exec_elf_images': inputs['exec_elf_images']}
            check(cases, 'legacy/' + label, lambda: validate(run['results'], run['mode'], True, **kwargs), True,
                  provenance='actual_archived_run', archived_validated=run['observation_validated'],
                  kernel_log_read_result=marker(run['results'], 'stock_init_kernel_log')['read_result'])
            if run['phase'] == 'recovery':
                check(cases, 'mounted_recovery_rejects_raw_profile/' + label,
                      lambda: validate(run['results'], run['mode'], True, first_stage=True, **kwargs), False)
                position = run['results'].index(marker(run['results'], 'stock_init_original_system_mounted'))
                mounted = run['results'][:position] + device_rows(False) + run['results'][position:]
                recovery_input = {**inputs, 'first_stage_contract': 1}

                def recovery_accepted(rows):
                    return validate(rows, run['mode'], True, guest_page_bytes=page, kernel_log=run['stderr'],
                                    original_input=recovery_input, exec_elf_images=inputs['exec_elf_images'])

                check(cases, 'recovery_sysfs/' + label, lambda: recovery_accepted(mounted), True,
                      provenance='actual_archived_recovery_with_actual_c_sysfs_and_synthetic_caller')
                missing = [row for row in mounted if 'stock_init_first_stage_sysfs_mount' not in row]
                early = move_before(mounted, 'stock_init_original_system_mounted', 'stock_init_first_stage_sysfs_mount')
                extra_unmount = mounted[:position] + [
                    {'stock_init_first_stage_sysfs_mount': True, 'operation': 'unmount', 'result': 0}] + mounted[position:]
                for name, changed in (('recovery_sysfs_mount_missing', missing),
                                      ('recovery_system_mount_before_sysfs', early),
                                      ('recovery_sysfs_unmount_extra', extra_unmount)):
                    check(cases, name + '/' + label, lambda changed=changed: recovery_accepted(changed), False)
                continue
            raw, manifest = raw_rows(run, ramdisk, spec, dm_cases['empty'], page)
            raw_input = {**inputs, 'first_stage_contract': 1,
                         'sdk_ramdisk': {'entries': manifest}, 'exec_elf_images': {'/init': spec, **inputs['exec_elf_images']}}

            def accepted(rows):
                return validate(rows, run['mode'], True, guest_page_bytes=page, kernel_log=run['stderr'],
                                original_input=raw_input, exec_elf_images=raw_input['exec_elf_images'], first_stage=True)

            check(cases, 'raw/' + label, lambda: accepted(raw), True, provenance='synthetic_raw_with_actual_base')
            if report_name == 'init_16k' and run['entry'] == 'cli' and run['mode'] == 'lifecycle':
                check_ramdisk_endings(cases, raw, accepted, label)
            if report_name == 'init_4k' and run['entry'] == 'cli' and run['mode'] == 'original':
                check_table_schema(cases, raw, accepted, trace_results)
                check_sysfs_contract(cases, raw_input, raw, accepted, device_fixture)
            for denial in (-1, -13):
                check(cases, 'kernel_policy_' + str(denial) + '/' + label,
                      lambda denial=denial: accepted(replace(raw, 'stock_init_kernel_log', read_result=denial)),
                      run['mode'] == 'original')
            variants = [('sync_before_reaped', move_before(raw, 'stock_init_first_stage_sync', 'stock_init_host_stop_reaped')),
                        ('prepare_after_ready', move_before(raw, 'stock_init_first_stage_dm_prepare', 'stock_init_observation')),
                        ('shutdown_before_fd_logs', move_before(raw, 'stock_init_shutdown', 'stock_init_output')),
                        ('kernel_log_read_failed', replace(raw, 'stock_init_kernel_log', read_result=-5)),
                        ('parent_fd_read_failed', replace(raw, 'stock_init_output', read_result=-9)),
                        ('parent_fd_wrong_number', replace(raw, 'stock_init_output', fd=3)),
                        ('ramdisk_content_failed', replace(raw, 'stock_init_ramdisk_entry', content_matches=False)),
                        ('ramdisk_entry_missing', [row for row in raw if row is not marker(raw, 'stock_init_ramdisk_entry')]),
                        ('mounted_row_in_raw', raw[:1] + [{'stock_init_data_mount': True}] + raw[1:])]
            if run['mode'] == 'original':
                variants.append(('original_exec_before_ready', move_before(raw, 'stock_init_exec', 'stock_init_ramdisk_ready')))
            for name, changed in variants:
                check(cases, name + '/' + label, lambda changed=changed: accepted(changed), False)
            if run['mode'] == 'lifecycle':
                natural = replace(raw, 'stock_init_observation', deadline_stop=False, status=0, kill_result=-3)
                natural = replace(natural, 'stock_init_stop_request', result=-3)
                natural = [row for row in natural if 'stock_init_host_stop' not in row]
                check(cases, 'raw_lifecycle_natural_exit/' + label, lambda: accepted(natural), False)
            for name in ('busy', 'buffer-full', 'invalid-start', 'short-dev', 'outside-size',
                         'remove-rejected', 'list-rejected', 'control-close-failed'):
                changed = [row for row in raw if not any(key.startswith('stock_init_first_stage_dm_') for key in row)]
                prep = [row for row in dm_cases[name] if row.get('phase') == 'prepare' or 'stock_init_first_stage_dm_prepare' in row]
                finish = [row for row in dm_cases[name] if row.get('phase') == 'cleanup' or 'stock_init_first_stage_dm_cleanup' in row]
                changed[:0] = prep
                offset = changed.index(marker(changed, 'stock_init_first_stage_sync')) + 1
                changed[offset:offset] = finish
                check(cases, 'dm_' + name + '/' + label, lambda changed=changed: accepted(changed), False,
                      provenance='actual_dm_host_fixture_rows_in_synthetic_raw')
        for numeric in (False, True):
            original = next(run for run in report['runs'] if run['mode'] == 'original' and run['entry'] == 'cli')
            dynamic_pair = exec_rows(original['results'])[:2]
            fstab = next(entry for entry in ramdisk['entries'] if entry['path'] == 'first_stage_ramdisk/fstab.ranchu')['data'].decode()
            partition_order = list(dict.fromkeys(fields[0] for line in fstab.splitlines()
                                                if (fields := line.split()) and fields[0] in inputs['sdk_partitions']))
            rows = progress_rows(inputs, dynamic_pair, partition_order, numeric)
            suffix = report_name + ('/numeric' if numeric else '/path')
            check(cases, 'progress_complete/' + suffix, lambda: progress(rows, inputs)['completed'], True,
                  provenance='synthetic_syscall_progress_with_actual_lp')
            devices = observe_devices(rows, inputs, True)
            mapper_nodes = [row for row in rows if row.get('stock_init_block') == 33 and
                            bytes.fromhex(row['paths_hex'][0]).decode().startswith('/dev/block/dm-')]
            majors = [('current', [253] * len(mapper_nodes), True),
                      ('alternate', [252] * len(mapper_nodes), True),
                      ('zero', [0] * len(mapper_nodes), False),
                      ('nce_whole', [devices['ncedisk1']['major']] * len(mapper_nodes), False),
                      ('gpt_partition', [devices['ncedisk1p2']['major']] * len(mapper_nodes), False),
                      ('mixed', [252] + [253] * (len(mapper_nodes) - 1), False)]
            for name, values, valid in majors:
                changed = copy.deepcopy(rows)
                nodes = [row for row in changed if row.get('stock_init_block') == 33 and
                         bytes.fromhex(row['paths_hex'][0]).decode().startswith('/dev/block/dm-')]
                for node, major in zip(nodes, values, strict=True):
                    node['args'][3] = (node['args'][3] & ~(4095 << 8)) | (major << 8)
                observed = progress(changed, inputs)
                check(cases, 'mapper_major_' + name + '/' + suffix,
                      lambda observed=observed, valid=valid: observed['mapper_device_class_valid'] is valid and
                      observed['completed'] is valid, True, expected_class_valid=valid, expected_completed=valid,
                      major_values=values, observed_class_valid=observed['mapper_device_class_valid'],
                      observed_completed=observed['completed'], provenance='synthetic_mapper_major_only')
            checks = [('metadata_mount_missing', lambda row: row.get('stock_init_block') == 40 and row['paths_hex'][1] == b'/metadata'.hex()),
                      ('lp_table_missing', lambda row: row.get('stock_init_dm_table_load') is True),
                      ('partition_mount_missing', lambda row: row.get('stock_init_block') == 40 and row['paths_hex'][1] == b'/vendor'.hex()),
                      ('switch_root_missing', lambda row: row.get('stock_init_block') == 51),
                      ('dynamic_exec_missing', lambda row: row.get('stock_init_security') == 221)]
            for name, predicate in checks:
                missing = next(row for row in rows if predicate(row))
                changed = [row for row in rows if row is not missing]
                check(cases, name + '/' + suffix, lambda changed=changed: progress(changed, inputs)['completed'], False)
            wrong = copy.deepcopy(rows)
            metadata_mount = next(row for row in wrong if row.get('stock_init_block') == 40 and row['paths_hex'][1] == b'/metadata'.hex())
            metadata_mount['paths_hex'][0] = b'/dev/block/wrong-device'.hex()
            check(cases, 'metadata_wrong_source/' + suffix, lambda: progress(wrong, inputs)['completed'], False)
            wrong = copy.deepcopy(rows)
            partition_mount = next(row for row in wrong if row.get('stock_init_block') == 40 and row['paths_hex'][1] == b'/vendor'.hex())
            partition_mount['paths_hex'][0] = b'/dev/block/wrong-device'.hex()
            check(cases, 'lp_mount_wrong_source/' + suffix, lambda: progress(wrong, inputs)['completed'], False)
            early_mount = copy.deepcopy(rows)
            mount = next(row for row in early_mount if row.get('stock_init_block') == 40 and row['paths_hex'][1] == b'/vendor'.hex())
            early_mount.remove(mount)
            position = next(index for index, row in enumerate(early_mount) if row.get('stock_init_block') == 51)
            early_mount.insert(position, mount)
            check(cases, 'partition_mount_before_chroot/' + suffix, lambda: progress(early_mount, inputs)['completed'], False)
            for name in ('ncedisk1p2', 'ncedisk2p1'):
                wrong = copy.deepcopy(rows)
                node = next(row for row in wrong if row.get('stock_init_block') == 33 and
                            row['paths_hex'][0] == ('/dev/block/' + name).encode().hex())
                node['args'][3] += 1
                check(cases, 'block_device_differs_sysfs_' + name + '/' + suffix,
                      lambda wrong=wrong: progress(wrong, inputs)['completed'], False)
            if numeric:
                wrong = copy.deepcopy(rows)
                marker(wrong, 'stock_init_dm_table_load')['targets'][0]['backing_minor'] += 1
                check(cases, 'numeric_backing_differs_sysfs/' + suffix, lambda: progress(wrong, inputs)['completed'], False)
            if report_name == 'init_4k' and not numeric:
                devices = observe_devices(rows, inputs, True)
                next_minor = max(item['minor'] for item in devices.values() if item['major'] == 259) + 1
                for name in ('ncedisk1p2', 'ncedisk2p1'):
                    path = '/sys/class/block/' + name
                    device = devices[name]
                    wrong = sysfs_content(rows, path + '/dev', f"{device['major']}:{next_minor}\n".encode())
                    event = bytes.fromhex(sysfs_query(rows, path + '/uevent')['value_hex'])
                    wrong = sysfs_content(wrong, path + '/uevent', event.replace(
                        f"MINOR={device['minor']}\n".encode(), f"MINOR={next_minor}\n".encode()))
                    check(cases, 'sysfs_changed_minor_valid/' + name,
                          lambda wrong=wrong: validate_devices(wrong, inputs, True), True,
                          provenance='synthetic_distinct_minor_with_matching_dev_and_uevent')
                    check(cases, 'sysfs_changed_minor_differs_block/' + name,
                          lambda wrong=wrong: progress(wrong, inputs)['completed'], False)
            early_exec = move_before(rows, 'stock_init_security', 'stock_init_dm_table_load')
            early_exec = move_before(early_exec, 'stock_init_exec_layout', 'stock_init_dm_table_load')
            check(cases, 'dynamic_exec_before_root_change/' + suffix, lambda: progress(early_exec, inputs)['completed'], False)
            child_exec = replace(rows, 'stock_init_exec_layout', pid=2)
            check(cases, 'dynamic_exec_not_pid_one/' + suffix, lambda: progress(child_exec, inputs)['completed'], False)
            layout_first = move_before(rows, 'stock_init_exec_layout', 'stock_init_security')
            check(cases, 'dynamic_layout_before_security/' + suffix, lambda: progress(layout_first, inputs)['completed'], False)
            mismatched = replace(rows, 'stock_init_exec_layout', exec_index=marker(rows, 'stock_init_security')['exec_index'] + 1)
            check(cases, 'dynamic_exec_index_mismatch/' + suffix, lambda: progress(mismatched, inputs)['completed'], False)
            unreadable = replace(rows, 'stock_init_exec_layout', read_result=-14)
            check(cases, 'dynamic_layout_unreadable/' + suffix, lambda: progress(unreadable, inputs)['completed'], False)
            stale = copy.deepcopy(rows)
            mapper = next(row for row in stale if row.get('stock_init_block') == 36 and row['paths_hex'][1] == b'/dev/block/mapper/vendor'.hex())
            mapper['paths_hex'][0] = b'/dev/block/dm-wrong'.hex()
            check(cases, 'lp_mapper_wrong_node/' + suffix, lambda: progress(stale, inputs)['completed'], False)
        if report_name == 'init_4k':
            check_completion(cases, report, progress(rows, inputs))
    result = {'test': 'stock_init_first_stage_validator_review', 'passed': all(case['passed'] for case in cases),
              'guest_executed': False, 'images_written': False, 'cases': cases,
              'sources': {'archive': identity, 'dm_fixture': {'path': str(args.dm_results.resolve()), 'sha256': digest(args.dm_results)},
                          'trace_fixture': {'path': str(args.trace_results.resolve()), 'sha256': digest(args.trace_results)},
                          'sysfs_fixtures': [{'path': str(DEVICE_FIXTURES / name), 'sha256': digest(DEVICE_FIXTURES / name)}
                                             for name in ('raw-baseline.json', 'default-baseline.json', 'result.json')],
                          'ramdisk': {key: ramdisk[key] for key in ('input', 'input_sha256', 'decoded_sha256')}},
              'source_sha256': initial_sha,
              'source_unchanged': all(digest(ROOT / name) == expected for name, expected in initial_sha.items()),
              'scope': '真实两页旧入口回放；sysfs 行来自实际 C fixture，设备值和返回为合成宿主输入；caller、raw 部署和进度为合成，动态 exec 和停止使用旧归档。'}
    result['passed'] &= result['source_unchanged']
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': len(cases),
                      'failed': [case['case'] for case in cases if not case['passed']]}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
