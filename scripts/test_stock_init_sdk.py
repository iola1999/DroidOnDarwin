#!/usr/bin/env python3
"""用实际 SDK36 LP 启动报告检查原盘范围、只读访问与 loop 清除。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import struct
import tempfile

from prepare_android_runtime import partition_extents
from probe_stock_init import ROOT, validate
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
    for run in report['runs']:
        assert run['observation_validated'] and validate(run['results'], run['mode'], True,
                                                       guest_page_bytes=page_bytes, kernel_log=run['stderr'], original_input=prepared)
    cases.append('actual_sdk36_entries_and_readonly_loops')
    original = next(run for run in report['runs'] if run['mode'] == 'lifecycle')

    def rejected(name, key, field=None, value=None):
        rows = deepcopy(original['results'])
        row = next(row for row in rows if key in row and not row.get('temporary'))
        if field is None:
            rows.remove(row)
        else:
            row[field] = value
        assert not validate(rows, 'lifecycle', True, guest_page_bytes=page_bytes,
                            kernel_log=original['stderr'], original_input=prepared), name
        cases.append(name)

    rejected('missing_loop', 'stock_init_sdk_loop')
    rejected('wrong_offset', 'stock_init_sdk_loop', 'offset', 0)
    rejected('wrong_sizelimit', 'stock_init_sdk_loop', 'sizelimit', 512)
    rejected('read_beyond_extent', 'stock_init_sdk_loop', 'end_read_result', 1)
    rejected('writable_loop', 'stock_init_sdk_loop', 'flags', 0)
    rejected('numeric_temporary', 'stock_init_sdk_loop', 'temporary', 0)
    rejected('missing_loop_release', 'stock_init_sdk_loop_released')
    rejected('still_bound_loop', 'stock_init_sdk_loop_released', 'status_result', 0)
    rejected('failed_loop_removal', 'stock_init_sdk_loop_released', 'remove_result', -16)
    rejected('accepted_block_write', 'stock_init_sdk_loop_lifecycle', 'block_write_result', 1)
    rejected('wrong_metadata_label', 'stock_init_original_metadata', 'label_hex', '00')
    rejected('wrong_partition_format', 'stock_init_original_partition_mounted', 'filesystem', 'tmpfs')
    rejected('missing_shutdown', 'stock_init_shutdown')
    rejected('failed_halt', 'stock_init_shutdown', 'halt_result', -1)
    rejected('unclosed_host_disks', 'stock_init_shutdown', 'host_disks_closed', False)
    rejected('numeric_halt', 'stock_init_shutdown', 'halt_result', False)
    originals = [run for run in report['runs'] if run['mode'] == 'original']
    for run in originals:
        for key, field, value in (('stock_init_sdk_cleanup_deferred', 'unmount_result', -5),
                                  ('stock_init_sdk_cleanup_deferred', 'closed_loop_fds', 0),
                                  ('stock_init_shutdown', 'host_disks_closed', False)):
            rows = deepcopy(run['results'])
            row = next(row for row in rows if key in row)
            row[field] = value
            assert not validate(rows, 'original', True, guest_page_bytes=page_bytes,
                                kernel_log=run['stderr'], original_input=prepared)
    if originals:
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
              'report': str(source), 'report_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'sha256': {str(Path(__file__).resolve().relative_to(ROOT)): hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                         str((ROOT / 'scripts/prepare_android_runtime.py').relative_to(ROOT)): hashlib.sha256((ROOT / 'scripts/prepare_android_runtime.py').read_bytes()).hexdigest(),
                         str((ROOT / 'scripts/stock_init_sdk.py').relative_to(ROOT)): hashlib.sha256((ROOT / 'scripts/stock_init_sdk.py').read_bytes()).hexdigest()}}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
