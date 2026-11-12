#!/usr/bin/env python3
"""检查完整桌面打包的执行参数、资源选择与输入变化拒绝。"""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from translation_package import digest, prepare


class PackageConfiguration(unittest.TestCase):
    def test_invalid_execution_settings_are_rejected_before_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = root / 'summary.json'
            cases = [{'guest_layout': value} for value in ('unknown', None, True, [], {})]
            cases += [{'data_tbi': value} for value in (None, 0, 1, '1')]
            cases += [{'cpus': True}, {'cpus': 3}, {'cpus': 2, 'guest_layout': 'low'},
                      {'cpus': 2, 'guest_layout': 'sparse'}, {'cpus': 2, 'data_tbi': True}]
            for fields in cases:
                with self.subTest(fields=fields), patch('translation_package.build_runtime') as build:
                    config = {'workload': 'desktop', 'execution': 'direct', 'code_memory': 'dual',
                              'cpus': 1, 'uaccess_stop': 'all', **fields}
                    report.write_text(json.dumps({'configuration': config}))
                    with self.assertRaisesRegex(ValueError, '客体地址布局、数据 TBI 或 CPU 组合无效'):
                        prepare(report, root / 'output', 'macos')
                    build.assert_not_called()
                    self.assertFalse((root / 'output').exists())

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


class PackageResources(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.report = self.root / 'summary.json'
        names = ['lkl-image.bin', 'system.img', 'android-data-seed.img', 'sigreturn.elf', 'guest.elf',
                 'late.elf', 'dynamic.elf', 'dash.elf', 'bionic-test.elf', 'libc.so', 'dependency.so',
                 'plugin.so', 'nce-policy.cil', 'property_info', 'ld.config.txt', 'NceWorkload.dex', 'enabled']
        for name in names:
            (self.root / name).write_bytes(name.encode())
        hashes = {str(self.root / name): digest(self.root / name) for name in names}
        passed = ['android_system_boot_completed', 'android_application_data_persistence', 'android_keystore_persistence',
                  'android_system_process_groups_stopped', 'linux_data_readonly_shutdown', 'android_clock_controls_ready',
                  'translation_prepared_code_execution', 'translated_context_cleanup', 'android_home_navigation']
        rows = [{'test': name, 'passed': True} for name in passed]
        rows += [{'test': 'android_desktop_visible_activity', 'package': name, 'passed': True}
                 for name in ('com.android.launcher3', 'com.android.deskclock', 'com.google.android.documentsui')]
        self.value = {'configuration': {'workload': 'desktop', 'execution': 'direct', 'code_memory': 'dual',
                                        'cpus': 1, 'uaccess_stop': 'all', 'mapping': 'pte', 'direct': {'memory': 'flat'}},
                      'sha256': hashes,
                      'data_seed': {'path': str(self.root / 'android-data-seed.img'),
                                    'sha256': hashes[str(self.root / 'android-data-seed.img')]}, 'runs': []}
        for entry, fresh, initial, final in [('cli', True, self.value['data_seed']['sha256'], 'after-cli'),
                                            ('gcd', False, 'after-cli', 'after-gcd')]:
            results = copy.deepcopy(rows)
            for row in results:
                if row['test'] in ('android_application_data_persistence', 'android_keystore_persistence'):
                    row['restored'] = not fresh
            self.value['runs'].append({'entry': entry, 'validated': True, 'returncode': 0, 'results': results,
                                       'data_image': {'path': str(self.root / 'persistent-data.img'), 'fresh': fresh,
                                                      'initial_sha256': initial, 'final_sha256': final, 'clean_shutdown': True}})
        library = self.root / 'libtranslation.a'
        library.write_bytes(b'library')
        self.build = {'library': str(library), 'library_sha256': digest(library), 'sha256': {}}

    def run_prepare(self, *, side_effect=None):
        self.report.write_text(json.dumps(self.value))
        with patch('translation_package.build_runtime', return_value=self.build, side_effect=side_effect):
            return prepare(self.report, self.root / 'output', 'macos')

    def test_verified_system_and_execution_identity(self):
        identities = set()
        for layout in ('host', 'low', 'sparse'):
            for tbi in (False, True):
                self.value['configuration'].update(guest_layout=layout, data_tbi=tbi)
                prepared = self.run_prepare()
                self.assertEqual(prepared['files']['system.img'], self.root / 'system.img')
                execution = prepared['metadata']['execution']
                self.assertEqual((execution['guest_layout'], execution['data_tbi']), (layout, tbi))
                identities.add(prepared['metadata']['runtime'])
        self.assertEqual(len(identities), 6)
        self.value['configuration'].pop('guest_layout')
        self.value['configuration'].pop('data_tbi')
        execution = self.run_prepare()['metadata']['execution']
        self.assertEqual((execution['guest_layout'], execution['data_tbi']), ('host', False))

    def test_candidate_selects_verified_image_and_rejects_changed_identity(self):
        directory = self.root / 'candidate'
        directory.mkdir()
        image = directory / 'system.img'
        image.write_bytes(b'original-runtime')
        self.value['sha256'][str(image)] = digest(image)
        candidate = {'image': {'path': str(image), 'sha256': digest(image), 'bytes': image.stat().st_size}}
        self.value['system_candidate'] = candidate
        with patch('translation_package.verify_candidate', return_value=candidate):
            self.assertEqual(self.run_prepare()['files']['system.img'], image)
        changed = copy.deepcopy(candidate)
        changed['image']['sha256'] = 'changed'
        with patch('translation_package.verify_candidate', return_value=changed):
            with self.assertRaisesRegex(ValueError, '候选身份与桌面验证记录不同'):
                self.run_prepare()
        self.value['sha256'].pop(str(image))
        with patch('translation_package.verify_candidate', return_value=candidate):
            with self.assertRaisesRegex(ValueError, '镜像未包含在验证记录中'):
                self.run_prepare()

    def test_ambiguous_or_unverified_images_are_rejected(self):
        second = self.root / 'second/system.img'
        second.parent.mkdir()
        second.write_bytes(b'second')
        self.value['sha256'][str(second)] = digest(second)
        with self.assertRaisesRegex(ValueError, '验证记录需要唯一的系统镜像'):
            self.run_prepare()
        self.value['sha256'].pop(str(second))
        seed = str(self.root / 'android-data-seed.img')
        expected = self.value['sha256'].pop(seed)
        with self.assertRaisesRegex(ValueError, '镜像未包含在验证记录中'):
            self.run_prepare()
        self.value['sha256'][seed] = expected
        self.value['sha256'].pop(str(self.root / 'guest.elf'))
        with self.assertRaisesRegex(ValueError, '资源未包含在验证记录中'):
            self.run_prepare()

    def test_changed_inputs_are_rejected_before_and_after_build(self):
        resource = self.root / 'guest.elf'
        original = resource.read_bytes()
        resource.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, '本机验证输入变化'):
            self.run_prepare()
        resource.write_bytes(original)

        def change_during_build(*_):
            resource.write_bytes(b'changed-during-build')
            return self.build

        with self.assertRaisesRegex(ValueError, '打包资源在编译期间变化'):
            self.run_prepare(side_effect=change_during_build)

    def test_cross_boot_data_digest_must_be_continuous(self):
        self.value['runs'][1]['data_image']['initial_sha256'] = 'unrelated'
        with self.assertRaisesRegex(ValueError, '跨启动数据盘摘要不连续'):
            self.run_prepare()


if __name__ == '__main__':
    unittest.main()
