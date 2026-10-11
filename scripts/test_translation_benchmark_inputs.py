#!/usr/bin/env python3
"""检查复用报告选用其实际验证过的内核变体。"""
import unittest

from benchmark_android_translation import ROOT, validated_kernel


class BenchmarkInputs(unittest.TestCase):
    def test_highres_report_selects_highres_image(self):
        path = 'work/native-linux-nce/lkl-translation-smp-kernel-highres/lkl-image.bin'
        report = {'sha256': {path: 'digest', 'scripts/probe_lkl_translation.py': 'source'}}
        self.assertEqual(validated_kernel(report), ROOT / path)

    def test_missing_or_ambiguous_kernel_is_rejected(self):
        for names in [[], ['normal/lkl-image.bin', 'highres/lkl-image.bin']]:
            with self.subTest(names=names), self.assertRaisesRegex(ValueError, '唯一'):
                validated_kernel({'sha256': {name: 'digest' for name in names}})


if __name__ == '__main__':
    unittest.main()
