#!/usr/bin/env python3
"""检查原始 runtime 身份及组件失败结果的解析边界。"""
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import stock_bionic as stock


def successful_rows():
    return ([{'component_supervisor': True, 'page_size': 16384}, {'component_start': 'linker_help'},
             {'component_stage': 'linker_help', 'started': True, 'success': True, 'exec_errno': 0,
              'exit_code': 0, 'signal': 0, 'usage': True}, {'component_start': 'dynamic'},
             {'component_main': True, 'aux_page_size': 16384, 'page_size': 16384}] +
            [{'component_check': name, 'success': True} for name in stock.CHECKS] +
            [{'component_stage': 'dynamic', 'started': True, 'success': True, 'exec_errno': 0,
              'exit_code': 0, 'signal': 0, 'usage': False},
             {'test': 'stock_bionic_observation_complete', 'passed': True}])


class Results(unittest.TestCase):
    def test_success(self):
        self.assertTrue(stock.validate(successful_rows()))

    def test_missing_and_duplicate_records(self):
        rows = successful_rows()
        for i in range(len(rows)):
            with self.subTest(remove=i):
                self.assertFalse(stock.validate(rows[:i] + rows[i + 1:]))
            with self.subTest(duplicate=i):
                self.assertFalse(stock.validate(rows[:i] + [rows[i]] + rows[i:]))

    def test_linker_signal_and_skipped_dynamic(self):
        rows = successful_rows()[:3]
        rows[-1].update(success=False, exit_code=-1, signal=6, usage=False)
        rows += [{'component_stage': 'dynamic', 'started': False, 'reason': 'linker_failed'},
                 {'test': 'stock_bionic_observation_complete', 'passed': True}]
        self.assertTrue(stock.validate(rows))
        rows[-2]['reason'] = 'arbitrary'
        self.assertFalse(stock.validate(rows))

    def test_exec_errno_and_main_failure(self):
        rows = successful_rows()
        for mode in ('exec', 'before_main', 'after_checks'):
            changed = copy.deepcopy(rows)
            changed[-2].update(success=False, exit_code=127 if mode == 'exec' else -1,
                               exec_errno=2 if mode == 'exec' else 0, signal=0 if mode == 'exec' else 11)
            if mode != 'after_checks':
                changed = changed[:4] + changed[-2:]
            self.assertTrue(stock.validate(changed), mode)

    def test_inconsistent_success_and_status(self):
        for field, value in (('success', False), ('exec_errno', 2), ('signal', 11),
                             ('exit_code', -1), ('started', 1)):
            rows = successful_rows()
            rows[-2][field] = value
            self.assertFalse(stock.validate(rows), field)
        rows = successful_rows()
        rows[6]['success'] = False
        self.assertFalse(stock.validate(rows))

    def test_stage_order_and_loaded_objects(self):
        original = successful_rows()
        for first in range(len(original)):
            for second in range(first + 1, len(original)):
                rows = copy.deepcopy(original)
                rows[first], rows[second] = rows[second], rows[first]
                self.assertFalse(stock.validate(rows), (first, second))
        rows = successful_rows()
        rows.insert(5, {'component_object': '/apex/com.android.runtime/lib64/bionic/libc.so', 'base': 0x200000})
        self.assertTrue(stock.validate(rows))
        rows[5]['base'] = -1
        self.assertFalse(stock.validate(rows))
        rows = successful_rows()
        rows[-1], rows[0] = rows[0], rows[-1]
        self.assertFalse(stock.validate(rows))


class Inputs(unittest.TestCase):
    def test_manifest_and_bytes(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            directory, out = root / 'runtime', root / 'out'
            out.mkdir()
            raw = bytearray(64)
            raw[:7] = b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HH', raw, 16, 3, 183)
            digest = hashlib.sha256(raw).hexdigest()
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps({'url': 'fixed-sdk', 'archive_sha1': 'fixed-archive',
                'files': {'com.android.runtime/' + path: digest for path in stock.FILES}}))
            for relative in stock.FILES:
                path = directory / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
            with patch.dict(stock.MANIFESTS, {'sdk36': stock.sha(manifest)}):
                identity = stock.prepare('sdk36', directory, manifest, out)
                self.assertTrue(stock.unchanged(identity))
                (directory / stock.FILES[1]).write_bytes(raw + b'changed')
                self.assertFalse(stock.unchanged(identity))
                with self.assertRaisesRegex(ValueError, '文件摘要'):
                    stock.prepare('sdk36', directory, manifest, out)
                manifest.write_text('{}')
                with self.assertRaisesRegex(ValueError, '清单摘要'):
                    stock.prepare('sdk36', directory, manifest, out)


if __name__ == '__main__':
    unittest.main()
