"""在临时 Git 仓库中检查格式化 Hook 的提交范围与失败行为。"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = b'int  value(void){return 1;}\n'
SWIFT_SOURCE = b'func value()->Int{return 1}\n'


@unittest.skipUnless(shutil.which('clang-format'), '需要 clang-format')
class PreCommitTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='droidondarwin-precommit-')
        self.addCleanup(directory.cleanup)
        self.repo = Path(directory.name)
        self.env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}
        for name in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE'):
            self.env.pop(name, None)
        self.git('init', '-q')
        self.git('config', 'user.name', 'Hook Test')
        self.git('config', 'user.email', 'hook-test@example.invalid')
        self.git('config', 'commit.gpgsign', 'false')
        for name in ('.githooks/pre-commit', 'scripts/format.sh', '.clang-format', '.clang-format-ignore', '.swiftformat'):
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target)
        self.git('add', '.')
        self.git('commit', '-qm', 'test setup')
        self.git('config', 'core.hooksPath', '.githooks')

    def git(self, *args, check=True, env=None):
        return subprocess.run(['git', '--literal-pathspecs', *args], cwd=self.repo,
                              env=env or self.env, capture_output=True, check=check)

    def write(self, name, data=SOURCE):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_formats_only_staged_files(self):
        selected = self.write('selected.c')
        other = self.write('other.c')
        self.git('add', 'selected.c')
        self.git('commit', '-qm', 'format')
        self.assertNotEqual(selected.read_bytes(), SOURCE)
        self.assertEqual(self.git('show', 'HEAD:selected.c').stdout, selected.read_bytes())
        self.assertEqual(other.read_bytes(), SOURCE)
        self.assertEqual(self.git('ls-files', 'other.c').stdout, b'')
        self.assertEqual(self.git('diff', '--name-only').stdout, b'')

    def test_partial_staging_aborts_before_any_write(self):
        first = self.write('first.c')
        partial = self.write('partial.c')
        self.git('add', 'first.c', 'partial.c')
        extra = SOURCE + b'int later(void){return 2;}\n'
        partial.write_bytes(extra)
        head = self.git('rev-parse', 'HEAD').stdout
        result = self.git('commit', '-qm', 'partial', check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('未暂存修改'.encode(), result.stderr)
        self.assertEqual(first.read_bytes(), SOURCE)
        self.assertEqual(partial.read_bytes(), extra)
        self.assertEqual(self.git('show', ':partial.c').stdout, SOURCE)
        self.assertEqual(self.git('rev-parse', 'HEAD').stdout, head)

    def test_literal_filenames(self):
        for name in ('a[1] file.c', '-option.c', 'line\nbreak.c'):
            with self.subTest(name=name):
                path = self.write(name)
                unrelated = self.write('a1 file.c')
                self.git('add', '--', name)
                self.git('commit', '-qm', 'filename')
                self.assertNotEqual(path.read_bytes(), SOURCE)
                self.assertEqual(self.git('show', 'HEAD:' + name).stdout, path.read_bytes())
                self.assertEqual(unrelated.read_bytes(), SOURCE)
                self.assertEqual(self.git('ls-files', 'a1 file.c').stdout, b'')

    def test_docs_do_not_require_formatter(self):
        self.write('note.md', b'paragraph\n')
        self.git('add', 'note.md')
        self.git('commit', '-qm', 'docs', env={**self.env, 'PATH': '/usr/bin:/bin'})

    def test_missing_formatter_blocks_c_commit(self):
        path = self.write('missing.c')
        self.git('add', 'missing.c')
        result = self.git('commit', '-qm', 'missing', check=False,
                          env={**self.env, 'PATH': '/usr/bin:/bin'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('缺少 clang-format'.encode(), result.stderr)
        self.assertEqual(path.read_bytes(), SOURCE)
        self.assertEqual(self.git('show', ':missing.c').stdout, SOURCE)

    def test_ignored_source_is_unchanged(self):
        path = self.write('third_party/vendor.c')
        self.git('add', 'third_party/vendor.c')
        self.git('commit', '-qm', 'vendor')
        self.assertEqual(path.read_bytes(), SOURCE)
        self.assertEqual(self.git('show', 'HEAD:third_party/vendor.c').stdout, SOURCE)

    def test_rename_and_delete(self):
        self.write('before.c')
        self.git('add', 'before.c')
        self.git('commit', '-qm', 'before')
        self.git('mv', 'before.c', 'after.cpp')
        self.git('commit', '-qm', 'rename')
        self.assertTrue((self.repo / 'after.cpp').is_file())
        self.git('rm', 'after.cpp')
        self.git('commit', '-qm', 'delete')
        self.assertEqual(self.git('ls-files', 'after.cpp').stdout, b'')

    def test_symlink_target_is_unchanged(self):
        target = self.write('target')
        (self.repo / 'link.c').symlink_to('target')
        self.git('add', 'link.c')
        self.git('commit', '-qm', 'link', env={**self.env, 'PATH': '/usr/bin:/bin'})
        self.assertEqual(target.read_bytes(), SOURCE)
        self.assertEqual(self.git('show', 'HEAD:link.c').stdout, b'target')

    @unittest.skipUnless(shutil.which('swiftformat'), '需要 swiftformat')
    def test_swift_and_c_commit(self):
        swift = self.write('value.swift', SWIFT_SOURCE)
        c = self.write('value.c')
        other = self.write('other.swift', SWIFT_SOURCE)
        self.git('add', 'value.swift', 'value.c')
        self.git('commit', '-qm', 'mixed')
        for name, path, source in [('value.swift', swift, SWIFT_SOURCE), ('value.c', c, SOURCE)]:
            self.assertNotEqual(path.read_bytes(), source)
            self.assertEqual(self.git('show', 'HEAD:' + name).stdout, path.read_bytes())
        self.assertEqual(other.read_bytes(), SWIFT_SOURCE)
        self.assertEqual(self.git('ls-files', 'other.swift').stdout, b'')

    def test_swift_partial_staging_aborts_before_c_write(self):
        c = self.write('first.c')
        swift = self.write('partial.swift', SWIFT_SOURCE)
        self.git('add', 'first.c', 'partial.swift')
        extra = SWIFT_SOURCE + b'let later = 2\n'
        swift.write_bytes(extra)
        result = self.git('commit', '-qm', 'partial swift', check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('未暂存修改'.encode(), result.stderr)
        self.assertEqual(c.read_bytes(), SOURCE)
        self.assertEqual(swift.read_bytes(), extra)
        self.assertEqual(self.git('show', ':partial.swift').stdout, SWIFT_SOURCE)

    def test_missing_swift_formatter_blocks_commit(self):
        path = self.write('value.swift', SWIFT_SOURCE)
        self.git('add', 'value.swift')
        result = self.git('commit', '-qm', 'missing swift', check=False,
                          env={**self.env, 'PATH': '/usr/bin:/bin'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('缺少 swiftformat'.encode(), result.stderr)
        self.assertEqual(path.read_bytes(), SWIFT_SOURCE)
        self.assertEqual(self.git('show', ':value.swift').stdout, SWIFT_SOURCE)

    @unittest.skipUnless(shutil.which('swiftformat'), '需要 swiftformat')
    def test_swift_paths_exclusion_and_no_width_wrap(self):
        name = 'a[1] file.swift'
        long_line = b'let message = "' + b'a' * 400 + b'"\n'
        self.write(name, SWIFT_SOURCE + long_line)
        vendor = self.write('third_party/vendor.swift', SWIFT_SOURCE)
        self.git('add', name, 'third_party/vendor.swift')
        self.git('commit', '-qm', 'swift paths')
        self.assertIn(long_line, self.git('show', 'HEAD:' + name).stdout)
        self.assertEqual(vendor.read_bytes(), SWIFT_SOURCE)
        self.assertEqual(self.git('show', 'HEAD:third_party/vendor.swift').stdout, SWIFT_SOURCE)


if __name__ == '__main__':
    unittest.main()
