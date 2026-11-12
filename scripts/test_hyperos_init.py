#!/usr/bin/env python3
"""检查原 init 清单中的 newc 边界和 rc 续行、服务触发解析。"""
from pathlib import Path
import tempfile
import unittest

from hyperos_ramdisk import parse_newc
from inspect_hyperos_input import ROOT
from prepare_hyperos_init import rc_records


def newc_member(name, data=b''):
    encoded = name.encode() + b'\0'
    fields = [9, 0o100640, 123456, 654321, 2, 17, len(data), 3, 4, 5, 6, len(encoded), 0]
    record = b'070701' + b''.join(f'{field:08x}'.encode() for field in fields) + encoded
    record += bytes(-len(record) % 4)
    record += data
    return record + bytes(-len(record) % 4)


class HyperosInitInput(unittest.TestCase):
    def test_newc_preserves_original_fields(self):
        entries, contents = parse_newc(newc_member('init', b'abc') + newc_member('TRAILER!!!'))
        entry = entries[0]
        self.assertEqual({k: entry[k] for k in ('path', 'original_inode', 'uid', 'gid', 'mode', 'nlink', 'mtime', 'bytes',
                                               'dev_major', 'dev_minor', 'rdev_major', 'rdev_minor')},
                         {'path': '/init', 'original_inode': 9, 'uid': 123456, 'gid': 654321, 'mode': 0o640,
                          'nlink': 2, 'mtime': 17, 'bytes': 3, 'dev_major': 3, 'dev_minor': 4,
                          'rdev_major': 5, 'rdev_minor': 6})
        self.assertEqual(contents['/init'], b'abc')

    def test_newc_rejects_invalid_archive(self):
        trailer = newc_member('TRAILER!!!')
        valid = newc_member('init', b'abc') + trailer
        cases = {'magic': b'000000' + valid[6:], 'header': valid[:60],
                 'content': newc_member('init', b'abc')[:-4],
                 'traversal': newc_member('../init') + trailer,
                 'duplicate': newc_member('init') + newc_member('init') + trailer,
                 'extra-archive': trailer + trailer}
        for name, data in cases.items():
            with self.subTest(case=name), self.assertRaises(ValueError):
                parse_newc(data)

    def test_rc_continuation_and_exec_start(self):
        (ROOT / 'work').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=ROOT / 'work') as directory:
            path = Path(directory) / 'init.rc'
            path.write_text('import /system/etc/init/hw/init.${ro.zygote}.rc\n'
                            'service probe /system/bin/probe \\\n'
                            '    --option value\n'
                            '    class main\n'
                            '    disabled\n'
                            'on post-fs-data\n'
                            '    exec_start probe\n'
                            'on property:probe.enabled=true\n'
                            '    start probe\n'
                            '    class_start main\n')
            services, imports = rc_records({'/system/etc/init/init.rc': {'source': str(path)}})
            self.assertEqual(len(services), 1)
            service = services[0]
            self.assertEqual(service['arguments'], ['--option', 'value'])
            self.assertEqual(service['classes'], ['main'])
            self.assertTrue(service['disabled'])
            self.assertEqual([action['trigger'] for action in service['declared_actions']],
                             ['on post-fs-data', 'on property:probe.enabled=true'])
            self.assertEqual(service['declared_actions'][0]['commands'][0]['text'], 'exec_start probe')
            self.assertEqual(imports[0]['text'], 'import /system/etc/init/hw/init.${ro.zygote}.rc')


if __name__ == '__main__':
    unittest.main()
