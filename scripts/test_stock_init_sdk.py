#!/usr/bin/env python3
"""用实际 SDK36 LP 启动报告检查原盘范围、只读访问与 DM 移除。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import struct
import tempfile

from prepare_android_runtime import partition_extents
from probe_stock_init import ROOT, validate
from exec_aslr import validate_exec_layout
from stock_init_metadata import read_partition_metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = args.report.resolve()
    report = json.loads(source.read_text())
    prepared = report['input']
    assert set(prepared['sdk_partitions']) == {'system', 'product', 'system_ext', 'vendor', 'system_dlkm'}
    assert prepared['sdk_partitions']['system_dlkm']['filesystem'] == 'erofs'
    assert all(prepared['sdk_partitions'][name]['filesystem'] == 'ext4' for name in ('system', 'product', 'system_ext', 'vendor'))
    assert {'/vendor/bin/hw/android.hardware.security.keymint-service', '/vendor/etc/vintf/manifest.xml',
            '/vendor/etc/init/android.hardware.security.keymint-service.rc'} <= set(prepared['expected_runtime_metadata'])
    page_bytes = report['source_configuration'].get('guest_page_bytes', 16384)
    cases = []
    exec_association_runs = []
    for run in report['runs']:
        assert run['observation_validated'] and validate(run['results'], run['mode'], True,
                                                       guest_page_bytes=page_bytes, kernel_log=run['stderr'], original_input=prepared,
                                                       exec_elf_images=prepared.get('exec_elf_images'))
    cases.append('actual_sdk36_entries_and_readonly_dm')
    original = next(run for run in report['runs'] if run['mode'] == 'lifecycle')

    def rejected(name, key, field=None, value=None):
        rows = deepcopy(original['results'])
        row = next(row for row in rows if key in row or row.get('test') == key)
        if field is None:
            rows.remove(row)
        else:
            row[field] = value
        assert not validate(rows, 'lifecycle', True, guest_page_bytes=page_bytes,
                            kernel_log=original['stderr'], original_input=prepared,
                            exec_elf_images=prepared.get('exec_elf_images')), name
        cases.append(name)

    rejected('missing_dm', 'stock_init_sdk_dm')
    rejected('wrong_backing_device', 'stock_init_sdk_dm', 'backing_device', 65026)
    rejected('wrong_backing_start', 'stock_init_sdk_dm', 'backing_start_sector', 0)
    rejected('wrong_dm_length', 'stock_init_sdk_dm', 'sectors', 1)
    rejected('dm_nonzero_start', 'stock_init_sdk_dm', 'sector_start', 1)
    rejected('read_beyond_extent', 'stock_init_sdk_dm', 'end_read_result', 1)
    rejected('writable_dm', 'stock_init_sdk_dm', 'flags', 32)
    rejected('numeric_device', 'stock_init_sdk_dm', 'device', False)
    rejected('wrong_target_type', 'stock_init_sdk_dm', 'target', 'zero')
    rejected('missing_dm_removal', 'stock_init_sdk_dm_removed')
    rejected('still_registered_dm', 'stock_init_sdk_dm_removed', 'status_result', 0)
    rejected('failed_dm_removal', 'stock_init_sdk_dm_removed', 'remove_result', -16)
    rejected('accepted_block_write', 'stock_init_sdk_readonly', 'block_write_result', 1)
    rejected('wrong_metadata_label', 'stock_init_original_metadata', 'label_hex', '00')
    rejected('wrong_partition_format', 'stock_init_original_partition_mounted', 'filesystem', 'tmpfs')
    rejected('missing_shutdown', 'stock_init_shutdown')
    rejected('failed_halt', 'stock_init_shutdown', 'halt_result', -1)
    rejected('unclosed_host_disks', 'stock_init_shutdown', 'host_disks_closed', False)
    rejected('numeric_halt', 'stock_init_shutdown', 'halt_result', False)
    rejected('missing_context_cleanup', 'translated_context_cleanup')
    rejected('failed_context_cleanup', 'translated_context_cleanup', 'passed', False)
    rejected('incomplete_context_cleanup', 'translated_context_cleanup', 'freed', 1)
    if prepared.get('exec_elf_images'):
        rejected('nonzero_lifecycle_exec_count', 'translated_context_cleanup', 'stock_init_exec_count', 1)
        rejected('boolean_lifecycle_exec_count', 'translated_context_cleanup', 'stock_init_exec_count', False)
        rows = deepcopy(original['results'])
        summary = next(row for row in rows if row.get('test') == 'translated_context_cleanup')
        del summary['stock_init_exec_count']
        assert not validate(rows, 'lifecycle', True, guest_page_bytes=page_bytes,
                            kernel_log=original['stderr'], original_input=prepared, exec_elf_images=prepared['exec_elf_images'])
        cases.append('missing_lifecycle_exec_count')
    originals = [run for run in report['runs'] if run['mode'] == 'original']
    for run in originals:
        rows = run['results']
        property_path = '/system/etc/selinux/plat_property_contexts'.encode().hex()
        opened = next(i for i, row in enumerate(rows)
                      if row.get('stock_init_security') == 56 and row.get('path_hex') == property_path)
        fd = rows[opened]['result']
        assert fd >= 0
        first_read = next(row for row in rows[opened + 1:]
                          if row.get('stock_init_syscall') == 63 and row['args'][0] == fd)
        assert first_read['result'] > 0
        keymint_path = '/vendor/bin/hw/android.hardware.security.keymint-service'.encode().hex()
        keymint_execs = [row for row in rows
                        if row.get('stock_init_security') == 221 and row.get('path_hex') == keymint_path]
        assert keymint_execs and all(row['result'] == 0 for row in keymint_execs)
        for instance in ('android.hardware.security.keymint.IKeyMintDevice/default',
                         'android.hardware.security.secureclock.ISecureClock/default',
                         'android.hardware.security.sharedsecret.ISharedSecret/default',
                         'android.hardware.security.keymint.IRemotelyProvisionedComponent/default'):
            assert any(instance in line and ',uid=9999,sid=u:r:hal_keymint_default:s0)' in line
                       for line in run['guest_write_text'].splitlines())
        for key, field, value in (('stock_init_sdk_cleanup_deferred', 'unmount_result', -5),
                                  ('stock_init_sdk_cleanup_deferred', 'closed_partition_fds', 0),
                                  ('stock_init_shutdown', 'host_disks_closed', False),
                                  ('translated_context_cleanup', 'passed', False),
                                  ('translated_context_cleanup', 'freed', 0)):
            rows = deepcopy(run['results'])
            row = next(row for row in rows if key in row or row.get('test') == key)
            row[field] = value
            assert not validate(rows, 'original', True, guest_page_bytes=page_bytes,
                                kernel_log=run['stderr'], original_input=prepared,
                                exec_elf_images=prepared.get('exec_elf_images'))
        if prepared.get('exec_elf_images'):
            source_rows = run['results']
            layout_positions = [i for i, row in enumerate(source_rows) if 'stock_init_exec_layout' in row]
            security_positions = [i for i, row in enumerate(source_rows)
                                  if row.get('stock_init_security') == 221 and row.get('result') == 0 and
                                  row.get('path_hex') == '/system/bin/init'.encode().hex()]
            assert len(layout_positions) == len(security_positions) >= 3
            guest = next(row for row in source_rows if row.get('config') == 'translation_guest_layout')
            assert validate_exec_layout(source_rows, prepared['exec_elf_images'], guest, page_bytes)
            layout = source_rows[layout_positions[0]]
            variants = [([row for row in run['results'] if row is not layout], 'missing_actual_exec_layout'),
                        (run['results'] + [deepcopy(layout)], 'duplicate_actual_exec_layout'),
                        ([row for row in run['results'] if row is not layout] + [deepcopy(layout)],
                         'missing_exec_layout_replaced_by_later_duplicate'),
                        ([deepcopy(layout)] + [row for row in run['results'] if row is not layout],
                         'layout_before_successful_exec')]
            changed = list(source_rows)
            changed[layout_positions[1]] = deepcopy(layout)
            variants.append((changed, 'replace_second_layout_with_first_at_original_position'))
            for positions, name in ((layout_positions, 'swap_layouts_at_original_positions'),
                                    (security_positions, 'swap_security_at_original_positions')):
                changed = list(source_rows)
                changed[positions[0]], changed[positions[1]] = changed[positions[1]], changed[positions[0]]
                variants.append((changed, name))
            changed = list(source_rows)
            for positions in (layout_positions, security_positions):
                changed[positions[0]], changed[positions[1]] = changed[positions[1]], changed[positions[0]]
            variants.append((changed, 'swap_complete_exec_pairs_at_original_positions'))
            removed = {layout_positions[1], security_positions[1]}
            variants.append(([row for i, row in enumerate(source_rows) if i not in removed],
                             'missing_middle_exec_pair'))
            removed = {layout_positions[-1], security_positions[-1]}
            variants.append(([row for i, row in enumerate(source_rows) if i not in removed],
                             'missing_last_exec_pair'))
            summary_position = next(i for i, row in enumerate(source_rows)
                                    if row.get('test') == 'translated_context_cleanup')
            summary = source_rows[summary_position]
            variants.append(([row for i, row in enumerate(source_rows) if i != summary_position],
                             'missing_exec_summary'))
            variants.append((source_rows + [deepcopy(summary)], 'duplicate_exec_summary'))
            observation_position = next(i for i, row in enumerate(source_rows) if 'stock_init_observation' in row)
            for position, name in ((security_positions[-1], 'summary_before_last_exec'),
                                   (observation_position, 'summary_before_wait_observation'),
                                   (len(source_rows), 'summary_after_final_result')):
                changed = [row for i, row in enumerate(source_rows) if i != summary_position]
                changed.insert(position, deepcopy(summary))
                variants.append((changed, name))
            for value, name in ((None, 'missing'), (False, 'boolean'), (3.0, 'float'), ('3', 'string'),
                                (0, 'zero'), (-1, 'negative'), (len(layout_positions) - 1, 'too_small'),
                                (len(layout_positions) + 1, 'too_large'), (1 << 64, 'overflow')):
                changed = list(source_rows)
                changed[summary_position] = deepcopy(summary)
                if value is None:
                    changed[summary_position].pop('stock_init_exec_count')
                else:
                    changed[summary_position]['stock_init_exec_count'] = value
                variants.append((changed, name + '_actual_exec_count'))
            for position, kind in ((layout_positions[0], 'layout'), (security_positions[0], 'security')):
                for value, name in ((None, 'missing'), (False, 'boolean'), (1.0, 'float'), ('1', 'string'),
                                    (0, 'zero'), (-1, 'negative'), (2, 'skipped'), (1 << 64, 'overflow')):
                    changed = list(source_rows)
                    changed[position] = deepcopy(source_rows[position])
                    if value is None:
                        changed[position].pop('exec_index')
                    else:
                        changed[position]['exec_index'] = value
                    variants.append((changed, name + '_actual_exec_' + kind + '_index'))
            for field, value in (('phdr', 0), ('entry', layout['entry'] + 4), ('pc', layout['pc'] + 4),
                                 ('at_base', layout['at_base'] + page_bytes), ('page_bytes', 1), ('phnum', 1),
                                 ('read_result', -14), ('phdr_mapping', 0), ('entry_mapping', 1), ('pc_mapping', 3),
                                 ('entry_mapping', (1 << 30) | 5)):
                changed = list(source_rows)
                changed[layout_positions[0]] = {**layout, field: value}
                variants.append((changed, 'invalid_actual_exec_' + field))
            changed = list(source_rows)
            for i, row in enumerate(changed):
                if row.get('stock_init_security') == 221 and row.get('path_hex') == '/system/bin/init'.encode().hex():
                    changed[i] = {key: value for key, value in row.items() if key != 'exec_index'}
                    changed[i]['result'] = -13
            variants.append((changed, 'failed_original_init_exec'))
            for rows, name in variants:
                assert not validate_exec_layout(rows, prepared['exec_elf_images'], guest, page_bytes), name
                assert not validate(rows, 'original', True, guest_page_bytes=page_bytes,
                                    kernel_log=run['stderr'], original_input=prepared,
                                    exec_elf_images=prepared['exec_elf_images']), name
            assert not validate(run['results'], 'original', True, guest_page_bytes=page_bytes,
                                kernel_log=run['stderr'], original_input=prepared)
            repeated = list(source_rows)
            second = source_rows[layout_positions[1]]
            repeated[layout_positions[1]] = {**layout, 'exec_index': second['exec_index'], 'pid': second['pid']}
            assert validate_exec_layout(repeated, prepared['exec_elf_images'], guest, page_bytes)
            assert validate(repeated, 'original', True, guest_page_bytes=page_bytes,
                            kernel_log=run['stderr'], original_input=prepared, exec_elf_images=prepared['exec_elf_images'])
            rejected_cases = sorted({name for _, name in variants})
            exec_association_runs.append({'entry': run['entry'], 'page_bytes': page_bytes,
                                          'actual_exec_count': summary['stock_init_exec_count'],
                                          'actual_exec_indices': [source_rows[i]['exec_index'] for i in layout_positions],
                                          'actual_exec_pids': [source_rows[i]['pid'] for i in layout_positions],
                                          'rejected_cases': rejected_cases, 'repeated_address_fixture_accepted': True})
            cases += rejected_cases + ['missing_original_elf_input', 'repeated_address_fixture_accepted']
    if originals:
        cases.append('original_property_read_keymint_and_vintf')
        cases.append('rejected_incomplete_original_shutdown')

    image = Path(prepared['original_system_image']['path'])
    layout = partition_extents(image)
    header_at = layout['super_offset'] + 12288
    with image.open('rb') as stream:
        stream.seek(header_at)
        header = stream.read(128)
        table_bytes = struct.unpack_from('<I', header, 44)[0]
        stream.seek(0)
        prefix = stream.read(header_at + 128 + table_bytes)
    table_at = header_at + 128
    po, pn, ps = struct.unpack_from('<III', header, 80)
    eo, en, es = struct.unpack_from('<III', header, 92)
    part_at = next(table_at + po + i * ps for i in range(pn)
                   if prefix[table_at + po + i * ps:table_at + po + i * ps + 36].rstrip(b'\0') == b'system')
    extent_at = table_at + eo + struct.unpack_from('<I', prefix, part_at + 40)[0] * es
    with tempfile.TemporaryDirectory(prefix='sdk36-lp-boundaries-', dir=ROOT / 'work/native-linux-nce') as temporary:
        def malformed(name, offset, fmt, value, checksums=True):
            data = bytearray(prefix)
            struct.pack_into(fmt, data, offset, value)
            if checksums:
                data[header_at + 48:header_at + 80] = hashlib.sha256(data[table_at:]).digest()
                data[header_at + 12:header_at + 44] = bytes(32)
                data[header_at + 12:header_at + 44] = hashlib.sha256(data[header_at:table_at]).digest()
            path = Path(temporary) / (name + '.img')
            path.write_bytes(data)
            with path.open('r+b') as stream:
                stream.truncate(image.stat().st_size)
            try:
                partition_extents(path)
            except RuntimeError:
                cases.append(name)
            else:
                raise AssertionError(name)

        malformed('invalid_gpt_table_range', 512 + 80, '<I', 0)
        malformed('changed_lp_header_digest', header_at + 12, '<B', prefix[header_at + 12] ^ 1, False)
        malformed('changed_lp_tables_digest', table_at, '<B', prefix[table_at] ^ 1, False)
        malformed('invalid_extent_index', part_at + 40, '<I', en + 1)
        malformed('extent_beyond_super', extent_at + 12, '<Q', image.stat().st_size // 512)
        malformed('extent_size_overflow', extent_at, '<Q', (1 << 64) - 1)
        malformed('nonlinear_extent', extent_at + 8, '<I', 1)
        malformed('unsupported_extent_device', extent_at + 20, '<I', 1)
    for offset in (-512, 1, True):
        try:
            read_partition_metadata(image, {}, [], offset=offset)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid ext4 offset')
    cases.append('rejected_invalid_metadata_offsets')
    result = {'test': 'stock_init_sdk36_lp_contract', 'passed': True, 'cases': cases,
              'exec_association_runs': exec_association_runs,
              'report': str(source), 'report_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'sha256': {str(Path(__file__).resolve().relative_to(ROOT)): hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                         str((ROOT / 'scripts/prepare_android_runtime.py').relative_to(ROOT)): hashlib.sha256((ROOT / 'scripts/prepare_android_runtime.py').read_bytes()).hexdigest(),
                         str((ROOT / 'scripts/stock_init_sdk.py').relative_to(ROOT)): hashlib.sha256((ROOT / 'scripts/stock_init_sdk.py').read_bytes()).hexdigest()}}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
