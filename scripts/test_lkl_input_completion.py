#!/usr/bin/env python3
"""检查 LKL 生成输入的完成时间与源码变化拒绝条件。"""
import os
from pathlib import Path
import tempfile
import unittest

from probe_lkl import changed_inputs


class InputCompletionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='lkl-input-completion-',
                                                     dir=Path(__file__).resolve().parents[1] / 'work')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'lkl-source'
        self.build = self.root / 'lkl-build'
        self.source.mkdir()
        self.build.mkdir()
        self.started_ns = 1_000_000_000
        self.prepared_ns = 2_000_000_000
        self.built_ns = 3_000_000_000

    def file(self, name, modified_ns):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('input\n')
        os.utime(path, ns=(modified_ns, modified_ns))
        return path

    def changed(self, sources):
        return changed_inputs(sources, self.started_ns,
                              ((self.source, self.prepared_ns), (self.build, self.built_ns)))

    def test_prepared_source_and_generated_build_inputs_are_accepted(self):
        inputs = [self.file('probe.py', self.started_ns),
                  self.file('lkl-source/drivers/android/ashmem.c', self.prepared_ns),
                  self.file('lkl-build/.config', self.built_ns),
                  self.file('lkl-build/drivers/android/.ashmem.o.cmd', self.built_ns),
                  self.file('lkl-build/drivers/android/ashmem.o', self.built_ns),
                  self.file('lkl-build/drivers/android/ashmem-inputs.d', self.built_ns),
                  self.file('lkl-build/include/generated/autoconf.h', self.built_ns)]
        self.assertEqual(self.changed(inputs), [])

    def test_repository_source_changes_are_rejected(self):
        path = self.file('probe.py', self.started_ns + 1)
        self.assertEqual(self.changed([path]), [path])

    def test_kernel_source_changes_during_build_and_validation_are_rejected(self):
        for modified_ns in (self.prepared_ns + 1, self.built_ns + 1):
            with self.subTest(modified_ns=modified_ns):
                path = self.file('lkl-source/drivers/android/ashmem.c', modified_ns)
                self.assertEqual(self.changed([path]), [path])

    def test_generated_inputs_changed_after_build_are_rejected(self):
        for name in ('.config', 'drivers/android/ashmem-inputs.d'):
            with self.subTest(name=name):
                path = self.file('lkl-build/' + name, self.built_ns + 1)
                self.assertEqual(self.changed([path]), [path])


if __name__ == '__main__':
    unittest.main()
