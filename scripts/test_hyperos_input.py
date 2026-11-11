#!/usr/bin/env python3
"""检查固定 HyperOS 分区解析的截断、摘要、范围和输出预算边界。"""
import hashlib
import io
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import inspect_hyperos_input as scan
from inspect_hyperos_files import elf_layout


def prefix(*, overlap=False, outside=False, target_type=0, name='system'):
    data = bytearray(scan.PREFIX_BYTES)
    base, total = 2 * 1024**2, 16 * 1024**2
    gpt = bytearray(128 * 128)
    gpt[:16] = bytes([1]) * 16
    struct.pack_into('<QQ', gpt, 32, base // 512, total // 512 - 34)
    gpt[56:66] = 'super'.encode('utf-16le')
    data[1024:1024 + len(gpt)] = gpt
    header = bytearray(92)
    header[:8] = b'EFI PART'
    struct.pack_into('<I', header, 12, 92)
    struct.pack_into('<QIII', header, 72, 2, 128, 128, zlib.crc32(gpt))
    struct.pack_into('<I', header, 16, zlib.crc32(header))
    data[512:604] = header
    geometry = bytearray(struct.pack('<II32sIII', 0x616c4467, 52, bytes(32), 65536, 2, 4096))
    geometry[8:40] = hashlib.sha256(geometry).digest()
    data[base + 4096:base + 4096 + 52] = geometry
    parts = b''.join(struct.pack('<36sIIII', item.encode(), 1, i, 1, 0)
                     for i, item in enumerate((name, 'vendor', 'system_dlkm')))
    extents = b''.join(struct.pack('<QIQI', 8, target_type if i == 0 else 0,
                                  total // 512 if outside else 2048 + (0 if overlap else i * 8), 0)
                       for i in range(3))
    groups, devices = bytes(48), bytes(64)
    tables = parts + extents + groups + devices
    lp = bytearray(struct.pack('<IHHI32sI32s', 0x414c5030, 10, 0, 128, bytes(32),
                               len(tables), hashlib.sha256(tables).digest()))
    offset = 0
    for table, count, size in ((parts, 3, 52), (extents, 3, 24), (groups, 1, 48), (devices, 1, 64)):
        lp.extend(struct.pack('<III', offset, count, size))
        offset += len(table)
    lp[12:44] = hashlib.sha256(lp).digest()
    data[base + 12288:base + 12288 + 128 + len(tables)] = lp + tables
    return data, total


class InputBoundaries(unittest.TestCase):
    def test_linear_partitions(self):
        data, total = prefix()
        result = scan.part_ranges(data, total)
        self.assertEqual([part['name'] for part in result], ['system', 'vendor', 'system_dlkm'])
        self.assertEqual(result[0]['ranges'], [{'source': 3 * 1024**2, 'length': 4096, 'target': 0}])

    def test_corrupt_metadata(self):
        data, total = prefix()
        for offset in (512, 1024, 2 * 1024**2 + 4096, 2 * 1024**2 + 12288,
                       2 * 1024**2 + 12288 + 128):
            with self.subTest(offset=offset):
                changed = bytearray(data)
                changed[offset] ^= 1
                with self.assertRaises(ValueError):
                    scan.part_ranges(changed, total)
        with self.assertRaises(ValueError):
            scan.part_ranges(data[:-1], total)
        with self.assertRaises(ValueError):
            scan.part_ranges(data, 1024 * 1024)

    def test_invalid_extents(self):
        for arguments in ({'overlap': True}, {'outside': True}, {'target_type': 1}, {'name': 'other'}):
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                scan.part_ranges(*prefix(**arguments))

    def test_copy_boundaries(self):
        payload = b'fixed image payload'
        info = {'size': len(payload), 'sha256': hashlib.sha256(payload).hexdigest()}
        scan.ROOT.joinpath('work').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scan.ROOT / 'work') as directory:
            root = Path(directory)
            result = scan.copy_member(io.BytesIO(payload), root / 'valid', info)
            self.assertEqual(result['sha256'], info['sha256'])
            for i, (content, budget) in enumerate(((payload[:-1], 100), (payload + b'x', 100),
                                                   (payload.replace(b'f', b'F'), 100), (payload, 1))):
                with self.subTest(i=i), self.assertRaises(ValueError):
                    scan.copy_member(io.BytesIO(content), root / str(i), info, budget=budget)
            self.assertFalse((root / '3').exists())

    def test_selected_partition_bytes(self):
        data, total = prefix()
        data.extend(bytes(total - len(data)))
        data[3 * 1024**2:3 * 1024**2 + 8192] = b'A' * 4096 + b'B' * 4096
        info = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        with tempfile.TemporaryDirectory(dir=scan.ROOT / 'work') as directory:
            for name, expected in (('system', b'A' * 4096), ('vendor', b'B' * 4096)):
                output = Path(directory) / name
                scan.copy_member(io.BytesIO(data), output, info, partitions=True, partition=name)
                self.assertEqual(output.read_bytes(), expected)

    def test_elf_permission_overlap(self):
        for page, overlap in ((4096, True), (16384, False)):
            data = bytearray(page * 2)
            data[:7] = b'\x7fELF\x02\x01\x01'
            struct.pack_into('<H', data, 18, 183)
            struct.pack_into('<Q', data, 32, 64)
            struct.pack_into('<HH', data, 54, 56, 2)
            struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0, 0, page, page, page)
            struct.pack_into('<IIQQQQQQ', data, 120, 1, 6, page, page, 0, page, page, page)
            result = elf_layout(data)
            self.assertTrue(all(p['congruent_16k'] for p in result['loads']))
            self.assertEqual(bool(result['permission_overlap_16k']), overlap)
            with self.assertRaises(ValueError):
                elf_layout(data[:140])


if __name__ == '__main__':
    unittest.main()
