#!/usr/bin/env python3
"""检查 ELF 执行布局，以及代码区内常量的指令分类。先构建 N2b 用户程序。"""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from build_nce_userspace import inspect, PAGE, WORK


class ElfLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = WORK / 'processes/worker.elf'
        if not cls.fixture.exists():
            raise RuntimeError('Run python3 scripts/probe_lkl.py --processes first')
        cls.original = cls.fixture.read_bytes()
        cls.llvm = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='elf-test-', dir=WORK)
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'worker.elf'
        self.data = bytearray(self.original)
        phoff, = struct.unpack_from('<Q', self.data, 32)
        phnum, = struct.unpack_from('<H', self.data, 56)
        self.headers = [(phoff + i * 56, struct.unpack_from('<IIQQQQQQ', self.data, phoff + i * 56))
                        for i in range(phnum)]
        self.rx = next((off, p) for off, p in self.headers if p[0] == 1 and p[1] & 1)

    def check_rejected(self, message):
        self.path.write_bytes(self.data)
        with self.assertRaisesRegex(RuntimeError, message):
            inspect(self.path)

    def test_large_text_and_data_words(self):
        elf = inspect(self.fixture)
        self.assertGreater(sum(size for _, size in elf['segments']), 65536)
        symbols = subprocess.check_output([self.llvm / 'llvm-nm', '-P', self.fixture], text=True)
        constant = next(line.split() for line in symbols.splitlines() if line.startswith('instruction_constants '))
        address = int(constant[2], 16)
        segment = next(p for _, p in self.headers if p[0] == 1 and p[3] <= address < p[3] + p[5])
        words = struct.unpack_from('<III', self.data, segment[2] + address - segment[3])
        self.assertEqual(words, (0xd4000001, 0xd53bd040, 0xd51bd040))
        patches = dict(elf['patches'])
        for i in range(3):
            self.assertNotIn(address + i * 4, patches)
        self.assertIn(words[0], patches.values())
        for word in words[1:]:
            self.assertIn(word, [instruction & 0xffffffe0 for instruction in patches.values()])

    def test_writable_executable_segment(self):
        off, p = self.rx
        struct.pack_into('<I', self.data, off + 4, p[1] | 2)
        self.check_rejected('writable executable segment')

    def test_code_and_data_share_host_page(self):
        off, writable = next((off, p) for off, p in self.headers if p[0] == 1 and p[1] & 2)
        rx = self.rx[1]
        overlap = ((rx[3] + rx[6] - 1) & -PAGE) + writable[2] % PAGE
        struct.pack_into('<Q', self.data, off + 16, overlap)
        self.check_rejected('share a host page')

    def test_missing_mapping_symbols(self):
        shutil.copyfile(self.fixture, self.path)
        subprocess.run([self.llvm / 'llvm-strip', '--strip-all', self.path], check=True)
        with self.assertRaisesRegex(RuntimeError, 'lacks mapping symbols'):
            inspect(self.path)

    def test_unsupported_alignment(self):
        struct.pack_into('<Q', self.data, self.rx[0] + 48, PAGE * 2)
        self.check_rejected('unsupported segment layout')

    def test_entry_in_executable_page_padding(self):
        rx = self.rx[1]
        self.assertNotEqual((rx[3] + rx[5]) % PAGE, 0)
        struct.pack_into('<Q', self.data, 24, rx[3] + rx[5])
        self.check_rejected('missing executable entry')

    def test_interpreter_needs_its_own_loader(self):
        off, _ = next((off, p) for off, p in self.headers if p[0] == 0x6474e551)
        struct.pack_into('<I', self.data, off, 3)
        self.check_rejected('interpreter loading')

    def test_truncated_section_table(self):
        self.data = self.data[:-1]
        self.check_rejected('truncated ELF tables')


if __name__ == '__main__':
    unittest.main(verbosity=2)
