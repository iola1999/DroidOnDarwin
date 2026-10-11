#!/usr/bin/env python3
"""检查完整桌面报告与 App 固定的用户复制策略一致。"""
import json
from pathlib import Path
import tempfile
import unittest

from translation_package import prepare


class PackageConfiguration(unittest.TestCase):
    def test_mismatched_or_missing_uaccess_is_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / 'summary.json'
            for cpus, strategy in [(2, 'all'), (1, 'code'), (2, None)]:
                with self.subTest(cpus=cpus, strategy=strategy):
                    config = {'workload': 'desktop', 'execution': 'direct',
                              'code_memory': 'dual', 'cpus': cpus}
                    if strategy is not None:
                        config['uaccess_stop'] = strategy
                    report.write_text(json.dumps({'configuration': config}))
                    with self.assertRaisesRegex(ValueError, '用户复制暂停策略与 App 不一致'):
                        prepare(report, root / 'output', 'macos')
                    self.assertFalse((root / 'output').exists())


if __name__ == '__main__':
    unittest.main()
