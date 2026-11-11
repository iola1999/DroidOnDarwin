#!/usr/bin/env python3
"""检查用户态修改和旧镜像不会进入翻译执行验证。"""
import json
from pathlib import Path
import tempfile
import unittest

from probe_lkl_translation import sha, validate_android_image


class ImageInputs(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.directory = self.root / 'image'
        self.directory.mkdir()
        self.source = self.root / 'experiments/native-linux/userspace/interaction-profile.cpp'
        names = [self.source, self.root / 'scripts/build_nce_userspace.py',
                 self.root / 'experiments/native-linux/input.h',
                 self.root / 'experiments/native-linux/interaction-profile.h',
                 self.root / 'experiments/native-linux/services/NceApkInstall.java']
        for path in names:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('original')
        (self.directory / 'system.img').write_bytes(b'image')
        manifest = {'input_sha256': {str(p.relative_to(self.root)): sha(p) for p in names},
                    'generated_sha256': {'system.img': sha(self.directory / 'system.img')}}
        (self.directory / 'lkl-summary.json').write_text(json.dumps(manifest))

    def test_matching_inputs_pass(self):
        validate_android_image(self.root, self.directory)

    def test_edited_guest_is_rejected(self):
        self.source.write_text('updated')
        with self.assertRaisesRegex(ValueError, 'interaction-profile.cpp'):
            validate_android_image(self.root, self.directory)

    def test_edited_apk_helper_is_rejected(self):
        helper = self.root / 'experiments/native-linux/services/NceApkInstall.java'
        helper.write_text('updated')
        with self.assertRaisesRegex(ValueError, 'NceApkInstall.java'):
            validate_android_image(self.root, self.directory)

    def test_added_and_removed_sources_are_rejected(self):
        added = self.source.with_name('input-service.cpp')
        added.write_text('new')
        with self.assertRaisesRegex(ValueError, 'input-service.cpp'):
            validate_android_image(self.root, self.directory)
        added.unlink()
        self.source.unlink()
        with self.assertRaisesRegex(ValueError, 'interaction-profile.cpp'):
            validate_android_image(self.root, self.directory)

    def test_changed_image_is_rejected(self):
        (self.directory / 'system.img').write_bytes(b'different image')
        with self.assertRaisesRegex(ValueError, 'system.img'):
            validate_android_image(self.root, self.directory)

    def test_host_executor_changes_do_not_require_guest_rebuild(self):
        host = self.root / 'experiments/native-linux/direct/decode.cpp'
        host.parent.mkdir()
        host.write_text('updated host executor')
        validate_android_image(self.root, self.directory)


if __name__ == '__main__':
    unittest.main()
