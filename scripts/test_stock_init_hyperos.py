#!/usr/bin/env python3
"""用固定输入和实际 SDK init 报告检查 HyperOS 挂载、标签与地址边界。"""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

from inspect_hyperos_input import ROOT
from probe_stock_init import validate
import stock_init_hyperos


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--sdk-init-report', type=Path, required=True)
    args = parser.parse_args()
    tests = []
    source = args.manifest.resolve()
    report = json.loads(args.sdk_init_report.read_text())
    page_bytes = report['source_configuration'].get('guest_page_bytes', 16384)
    assert page_bytes == 4096
    for run in report['runs']:
        assert run['observation_validated'] and validate(run['results'], run['mode'], True,
                                                       guest_page_bytes=page_bytes, kernel_log=run['stderr'],
                                                       original_input=report['input'] if report['input'].get('sdk_partitions') else None,
                                                       exec_elf_images=report['input'].get('exec_elf_images'))
    tests.append('actual_sdk36_four_entries')

    with tempfile.TemporaryDirectory(prefix='hyperos-init-contract-', dir=ROOT / 'work/native-linux-nce') as temporary:
        out = Path(temporary)
        prepared = stock_init_hyperos.prepare(source, out)
        assert len(prepared['files']) == 438 and len(prepared['original_root_links']) == 7
        assert len((out / 'original-metadata.tsv').read_text().splitlines()) == 13
        tests.append('fixed_hyperos_mount_input')

        def input_rejected(name, manifest=source):
            try:
                stock_init_hyperos.prepare(manifest, out)
            except ValueError:
                tests.append(name)
            else:
                raise AssertionError(name)

        changed = out / 'changed-manifest.json'
        changed.write_bytes(source.read_bytes() + b'\n')
        input_rejected('changed_manifest', changed)
        vendor = Path(prepared['original_vendor']['path'])
        original_is_file = Path.is_file
        with patch.object(Path, 'is_file', lambda path: path != vendor and original_is_file(path)):
            input_rejected('unavailable_original_vendor')
        selected = Path(next(iter(prepared['files'].values()))['source'])
        original_digest = stock_init_hyperos.digest
        with patch.object(stock_init_hyperos, 'digest', lambda path: '0' * 64 if path == selected else original_digest(path)):
            input_rejected('changed_original_file')

        # These rows extend an actual SDK lifecycle result; HyperOS execution is checked separately.
        rows = deepcopy(next(run['results'] for run in report['runs'] if run['mode'] == 'lifecycle'))
        layout = next(row for row in rows if row.get('config') == 'translation_guest_layout')
        layout['guest_end'] = 1 << 39
        layout['guest_span_bytes'] = layout['guest_end'] - layout['guest_base']
        rows = [row for row in rows if row.get('config') != 'translation_page_sizes']
        rows.append({'config': 'translation_page_sizes', 'guest_page_bytes': 4096,
                     'host_page_bytes': 16384, 'host_address_limit': 1 << 39})
        replaced = {'stock_init_root_symlink', 'stock_init_original_partition_mounted',
                    'stock_init_original_root_file', 'stock_init_original_metadata', 'stock_init_shutdown'}
        rows = [row for row in rows if not replaced.intersection(row) and
                not any(key.startswith('stock_init_sdk_') for key in row)]
        rows += [{'stock_init_root_symlink': True, 'path': path, 'target': item['link_target'],
                  'uid': item['uid'], 'gid': item['gid'], 'creation_mode': 0o777}
                 for path, item in sorted(prepared['original_root_links'].items())]
        rows += [{'stock_init_original_partition_mounted': True, 'path': path, 'filesystem': filesystem, 'readonly': True}
                 for path, filesystem in (('/product', 'bind'), ('/system_ext', 'bind'), ('/vendor', 'erofs'), ('/system_dlkm', 'erofs'))]
        rows.append({'stock_init_original_root_file': True, 'path': '/init.environ.rc', 'readonly': True})
        rows += [{'stock_init_original_metadata': True, 'path': path, 'uid': item['uid'], 'gid': item['gid'],
                  'mode': item['mode'], 'inode_type': item['inode_type'], 'label_hex': item['selinux_xattr_hex']}
                 for path, item in sorted(prepared['expected_runtime_metadata'].items())]
        trace = {'stock_init_syscall': 221, 'args': [0] * 6, 'pc': 1 << 37, 'result': -2}
        rows.append(trace)

        def observed(candidate, **kwargs):
            return validate(candidate, 'lifecycle', True, guest_page_bytes=4096, original_input=prepared, **kwargs)

        assert observed(rows)
        assert not validate(rows, 'lifecycle', True, original_input=prepared)
        assert not validate(rows, 'lifecycle', False, guest_page_bytes=4096, original_input=prepared)
        low_rows = deepcopy(rows)
        low_layout = next(row for row in low_rows if row.get('config') == 'translation_guest_layout')
        low_layout['guest_end'] = 1 << 36
        low_layout['guest_span_bytes'] = low_layout['guest_end'] - low_layout['guest_base']
        assert not observed(low_rows)
        low_rows[-1]['pc'] = 1 << 20
        assert observed(low_rows)
        tests += ['accepted_512gib_pc', 'rejected_wrong_guest_page', 'rejected_64gib_pc', 'accepted_64gib_lifecycle', 'rejected_unmounted_profile']

        def observation_rejected(name, key, field=None, value=None):
            candidate = deepcopy(rows)
            row = next(row for row in candidate if key in row)
            if field is None:
                candidate.remove(row)
            else:
                row[field] = value
            assert not observed(candidate), name
            tests.append(name)

        observation_rejected('missing_partition', 'stock_init_original_partition_mounted')
        observation_rejected('writable_partition', 'stock_init_original_partition_mounted', 'readonly', False)
        observation_rejected('numeric_readonly', 'stock_init_original_partition_mounted', 'readonly', 1)
        observation_rejected('wrong_filesystem', 'stock_init_original_partition_mounted', 'filesystem', 'ext4')
        observation_rejected('missing_root_link', 'stock_init_root_symlink')
        observation_rejected('wrong_root_link', 'stock_init_root_symlink', 'target', '/system/wrong')
        observation_rejected('missing_root_file', 'stock_init_original_root_file')
        observation_rejected('missing_metadata', 'stock_init_original_metadata')
        observation_rejected('wrong_label', 'stock_init_original_metadata', 'label_hex', '00')
        observation_rejected('wrong_owner', 'stock_init_original_metadata', 'uid', 123)
        original_mode = next(row['mode'] for row in rows if 'stock_init_original_metadata' in row)
        observation_rejected('wrong_mode', 'stock_init_original_metadata', 'mode', original_mode ^ 0o100)
        observation_rejected('wrong_inode_type', 'stock_init_original_metadata', 'inode_type', 0o040000)
        observation_rejected('numeric_metadata_owner', 'stock_init_original_metadata', 'uid', False)
        candidate = deepcopy(rows)
        candidate[-1]['pc'] = 1 << 39
        assert not observed(candidate)
        candidate[-1]['pc'] = 0xf000
        assert not observed(candidate)
        assert not observed([row for row in rows if row.get('config') != 'translation_guest_layout'])
        assert not observed(rows, kernel_log='BUG: non-zero pgtables_bytes on freeing mm:')
        tests += ['rejected_sparse_end_pc', 'rejected_guest_base_pc', 'rejected_missing_layout', 'rejected_page_table_warning']
    print(json.dumps({'test': 'stock_init_hyperos_contract', 'passed': True, 'cases': tests,
                      'hyperos_execution_validated': False}))


if __name__ == '__main__':
    main()
