#!/usr/bin/env python3
"""检查标准 ART 日志契约；构造的记录仅用于解析测试。"""
from copy import deepcopy
import json
from pathlib import Path
import tempfile

from stock_art import MAPS, TOKEN, artifacts, validate


def rows():
    check = lambda name, value: {'stock_art_check': name, 'passed': True, 'observed': value}
    maps = lambda name: {'stock_art_maps': name, 'bytes': 123}
    return [
        {'stock_art_supervisor': True, 'page_size': 16384, 'timeout_seconds': 90},
        {'stock_art_config': True, 'jni_version': 0x10006, 'jvalue_bytes': 8, 'jit_requested': True},
        maps('before-vm'), check('vm_absent', 0), {'stock_art_stage': 'create_vm'}, check('create_vm', 0),
        maps('after-vm'), check('load_dex', 1), check('primitive_arguments', TOKEN),
        check('registered_natives', TOKEN), check('gc_observed', 3), check('global_reference', TOKEN),
        maps('after-gc'), {'stock_art_array_storage': True, 'address': 0x1234000, 'is_copy': False},
        check('array_storage', 47), check('java_threads', 2000), check('native_thread', 58),
        check('java_exception', 1), check('warmup_result', sum(((i * 17) ^ (i >> 3)) + 9 for i in range(200000))),
        {'stock_art_jit': 'unverified', 'warmup_calls': 200000}, maps('after-warmup'),
        {'stock_art_stage': 'destroy_vm'}, check('destroy_vm', 0),
        {'test': 'stock_art_standard_jni', 'passed': True, 'jit_execution_confirmed': False},
        {'stock_art_exit': True, 'success': True, 'exec_errno': 0, 'exit_code': 0, 'signal': 0, 'timed_out': False},
        *[{'stock_art_export': name, 'present': True, 'bytes': 123} for name in MAPS],
    ]


def main():
    original = rows()
    assert validate(original)
    changed_page = deepcopy(original)
    changed_page[0]['page_size'] = 4096
    assert validate(changed_page, guest_page_bytes=4096)
    assert not validate(changed_page)
    assert not validate(original, guest_page_bytes=4096)
    assert not validate(original, guest_page_bytes=8192)
    rejected = 3
    for index, row in enumerate(original):
        assert not validate(original[:index] + original[index + 1:])
        assert not validate([*original, row])
        rejected += 2
        for key, value in row.items():
            modified = deepcopy(original)
            del modified[index][key]
            assert not validate(modified), (index, key)
            rejected += 1
            modified = deepcopy(original)
            modified[index][key] = not value if type(value) is bool and key != 'is_copy' else 'unexpected'
            assert not validate(modified), (index, key)
            rejected += 1
            if type(value) is int:
                modified[index][key] = True
                assert not validate(modified), (index, key)
                rejected += 1
    for reason in ('exec', 'signal', 'timeout'):
        modified = deepcopy(original)
        exit = next(row for row in modified if 'stock_art_exit' in row)
        exit.update(success=False, exec_errno=2 if reason == 'exec' else 0,
                    exit_code=127 if reason == 'exec' else -1,
                    signal=9 if reason == 'timeout' else 11 if reason == 'signal' else 0,
                    timed_out=reason == 'timeout')
        assert not validate(modified), reason
        rejected += 1
    parent = Path(__file__).resolve().parents[1] / 'work'
    with tempfile.TemporaryDirectory(prefix='stock-art-maps-test-', dir=parent) as directory:
        directory = Path(directory)
        missing = artifacts(directory, [{'stock_art_export': name, 'present': False} for name in MAPS])
        assert all(row['validated'] and not row['present'] for row in missing)
        for name in MAPS:
            (directory / ('maps-' + name + '.txt')).write_bytes(b'x' * 123)
        assert all(row['validated'] and row['present'] for row in artifacts(directory, original))
        (directory / 'maps-after-gc.txt').write_text('incomplete')
        assert not all(row['validated'] for row in artifacts(directory, original))
    print(json.dumps({'scope': 'Parser contract only; no ART execution', 'rejected_records': rejected,
                      'missing_and_changed_maps_checked': True}))


if __name__ == '__main__':
    main()
