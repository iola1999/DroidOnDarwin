"""检查离线产物在源码、类路径、DEX 和内容变化时失效。"""
import copy
import json
import struct
from pathlib import Path
import tempfile
import unittest

import art_aot


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='aot-test-', dir=art_aot.ROOT / 'work')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.inputs = {'art_source': 'same', 'offline_compiler': True}
        self.plan = {'targets': [art_aot.FIXTURE], 'input_files': {'/system/boot.jar': 'first'},
                     'recipe': {'compiler': 'first'}}
        for name in art_aot.file_names(self.plan):
            (self.directory / name).write_bytes(b'artifact')
        self.manifest = art_aot.write_manifest(self.directory, self.inputs, self.plan, [123])

    def test_runtime_without_compiler_can_load_matching_artifact(self):
        current = dict(self.inputs, offline_compiler=False)
        self.assertEqual(art_aot.validate(self.directory, current, self.plan), self.manifest)

    def test_changed_runtime_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'compiler, recipe or DEX'):
            art_aot.validate(self.directory, dict(self.inputs, art_source='changed'), self.plan)

    def test_changed_boot_dex_or_recipe_is_rejected(self):
        for field in ('input_files', 'recipe', 'targets'):
            with self.subTest(field=field):
                current = copy.deepcopy(self.plan)
                current[field] = [] if field == 'targets' else {'changed': 'new'}
                with self.assertRaisesRegex(RuntimeError, 'compiler, recipe or DEX'):
                    art_aot.validate(self.directory, self.inputs, current)

    def test_corrupt_code_is_rejected(self):
        (self.directory / 'NceWorkload.odex').write_bytes(b'changed')
        with self.assertRaisesRegex(RuntimeError, 'checksum'):
            art_aot.validate(self.directory, self.inputs, self.plan)

    def test_missing_or_unexpected_file_is_rejected_before_reading(self):
        for names in ({}, {'../unexpected': 'content'}):
            with self.subTest(names=names):
                self.manifest['files'] = names
                (self.directory / 'manifest.json').write_text(json.dumps(self.manifest))
                with self.assertRaisesRegex(RuntimeError, 'file set'):
                    art_aot.validate(self.directory, self.inputs, self.plan)

    def test_missing_heap_address_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'heap address'):
            art_aot.write_manifest(self.directory, self.inputs, self.plan, [])

    def test_boot_image_components_are_checked_independently_of_app_artifacts(self):
        plan = {'kind': 'boot', 'input_files': self.plan['input_files'], 'recipe': self.plan['recipe']}
        for name in art_aot.file_names(plan):
            (self.directory / name).write_bytes(b'boot')
        expected = art_aot.write_manifest(self.directory, self.inputs, plan, [123])
        self.assertEqual(art_aot.validate(self.directory, self.inputs, plan), expected)
        self.assertEqual(art_aot.validate(self.directory, self.inputs, self.plan), self.manifest)
        for name in ('boot.art', 'boot.oat', 'boot.vdex'):
            with self.subTest(file=name):
                (self.directory / name).write_bytes(b'changed')
                with self.assertRaisesRegex(RuntimeError, 'checksum'):
                    art_aot.validate(self.directory, self.inputs, plan)
                (self.directory / name).write_bytes(b'boot')


class LiteralPoolTests(unittest.TestCase):
    def words(self, *values):
        return struct.pack('<' + 'I' * len(values), *values)

    def test_code_after_pool_remains_executable(self):
        # The constants encode SVC, TLS and x18 instructions when disassembled as code.
        data = self.words(0x14000005, 0x5800009f, 0xd4000001, 0xd53bd040, 0xaa1203e0, 0xd65f03c0)
        self.assertEqual(art_aot.code_ranges(data, 0, 0, [(0, len(data))]), [(0, 4), (20, 24)])

    def test_tail_pool_preserves_real_x18_instruction_before_it(self):
        data = self.words(0xaa1203e0, 0xd65f03c0, 0x5800005f, 0x3d25ae4f)
        ranges = art_aot.code_ranges(data, 0, 0, [(0, len(data))])
        self.assertEqual(ranges, [(0, 8)])
        from build_nce_userspace import check_x18_disassembly
        with self.assertRaisesRegex(RuntimeError, 'unsupported use of x18'):
            check_x18_disassembly('fixture', {'patches': [], 'instruction_ranges': ranges},
                                  '0: aa1203e0 mov x0, x18')

    def test_pool_cannot_extend_past_method(self):
        for marker in (0x5800001f, 0x5800007f, 0x58ffffdf):
            with self.subTest(marker=hex(marker)), self.assertRaisesRegex(RuntimeError, 'pool bounds'):
                art_aot.code_ranges(self.words(0xd65f03c0, marker, 0), 0, 0, [(0, 12)])

    def test_throwing_call_can_precede_terminal_pool(self):
        data = self.words(0x94b97b7f, 0x5800005f, 0x42000000)
        self.assertEqual(art_aot.code_ranges(data, 0, 0, [(0, len(data))]), [(0, 4)])
        with self.assertRaisesRegex(RuntimeError, 'reachable fallthrough'):
            art_aot.code_ranges(data + self.words(0xd65f03c0), 0, 0, [(0, 16)])

    def test_fallthrough_into_pool_is_rejected(self):
        data = self.words(0xd503201f, 0x5800005f, 0)
        with self.assertRaisesRegex(RuntimeError, 'reachable fallthrough'):
            art_aot.code_ranges(data, 0, 0, [(0, len(data))])


if __name__ == '__main__':
    unittest.main()
