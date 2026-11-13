#!/usr/bin/env python3
"""用小目录检查实际克隆入口、共享 prefix 身份和 writer 的写入范围。"""
import argparse
import ast
from contextlib import ExitStack, redirect_stderr
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import plistlib
import stat
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.dont_write_bytecode = True
import build_lkl_translation_kernel as kernel
import build_translation_engine as engine
import build_translation_runtime as runtime
import probe_lkl_translation as core

ROOT = Path(__file__).resolve().parents[1]
FROZEN = {
    'scripts/build_lkl_translation_kernel.py': '6d9d158b45c2993f539bf1a4af40590bf9e156b80cd665902a0bfee9e658da2e',
    'scripts/probe_lkl.py': '362b485d5eb8ea04604d6b110f3b3a209ff9fb93bbcbff7dbbe47764f1c89e4f',
    'scripts/probe_lkl_translation.py': '347875d475f5fa64b52568e8ae432bd8b5f1b1edb6e17f538b270599a155fad1',
    'scripts/build_translation_runtime.py': 'e45a216be5a8d93c3154d9b8645191e8c5edad4fbb7a9a5f5a0f25088626f62a',
    'scripts/build_translation_engine.py': 'f5b4ac7a84139dc069eea77468bf7296a4e1ce09b61293dbd05b5596dac4ce36',
}


class BeforePatch(Exception):
    pass


class BeforeArchive(Exception):
    pass


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(directory):
    result = {}
    for path in [directory, *sorted(directory.rglob('*'))]:
        item = path.lstat()
        value = {'device': item.st_dev, 'inode': item.st_ino}
        if stat.S_ISLNK(item.st_mode):
            value.update(type='symlink', target=os.readlink(path))
        elif stat.S_ISREG(item.st_mode):
            value.update(type='file', content_hex=path.read_bytes().hex())
        else:
            value['type'] = 'directory'
        result[str(path.relative_to(directory))] = value
    return result


def record(cases, name, operation):
    try:
        cases.append({'case': name, **operation()})
    except (Exception, SystemExit) as error:
        cases.append({'case': name, 'passed': False, 'exception': {'type': type(error).__name__, 'message': str(error)}})


def filesystem_reader(calls):
    real_read = kernel.probe_lkl.subprocess.check_output

    def read(command, **kwargs):
        values = list(map(str, command))
        if values[:2] != ['df', '-P'] and values[:3] != ['diskutil', 'info', '-plist']:
            raise AssertionError('文件系统检查调用了其他宿主命令')
        row = {'command': values, 'timeout': kwargs.get('timeout')}
        calls.append(row)
        try:
            output = real_read(command, **kwargs)
        except kernel.subprocess.CalledProcessError as error:
            row.update(returncode=error.returncode, stdout=error.output.decode(errors='replace')
                       if isinstance(error.output, bytes) else error.output)
            raise
        row['returncode'] = 0
        if values[0] == 'df':
            row.update(directory=values[2], st_dev=Path(values[2]).stat().st_dev, stdout=output,
                       device=output.splitlines()[1].split()[0])
        else:
            info = plistlib.loads(output)
            row.update({name: info.get(name) for name in ('FilesystemName', 'DeviceIdentifier', 'MountPoint', 'VolumeUUID')})
        return output

    return read


def clone_case(directory, base, page, source_kind, expected_error=None):
    directory.mkdir()
    work = directory / 'work'
    if source_kind == 'external_work':
        actual_work = directory / 'shared-work'
        actual_work.mkdir()
        work.symlink_to(actual_work, target_is_directory=True)
    else:
        work.mkdir()
    (work / 'lkl-source').symlink_to(base, target_is_directory=True)
    output = work / 'translation-kernel'
    if page == 4096:
        output = output.with_name(output.name + '-4k-vmalloc')
    source = output / 'source'
    build_dir = output / 'build'
    shared = None
    original_file = None
    if source_kind in ('ancestor', 'out_internal'):
        shared = (directory if source_kind == 'ancestor' else work) / 'shared-output'
        (shared / 'source').mkdir(parents=True)
        (shared / 'source/old-input').write_bytes(b'existing shared fixture\n')
        (shared / 'output-input').write_bytes(b'existing output fixture\n')
        original_file = shared / 'source/old-input'
        output.symlink_to(shared, target_is_directory=True)
    elif source_kind in ('external_work', 'filesystem'):
        source.mkdir(parents=True)
        (source / 'old-input').write_bytes(b'existing work source fixture\n')
        (output / 'output-input').write_bytes(b'existing work output fixture\n')
        build_dir.mkdir()
        (build_dir / 'build-input').write_bytes(b'existing work build fixture\n')
        shared = actual_work if source_kind == 'external_work' else output
        original_file = source / 'old-input'
    elif source_kind in ('build_existing', 'build_dangling'):
        source.mkdir(parents=True)
        (source / 'old-input').write_bytes(b'existing source fixture\n')
        (output / 'output-input').write_bytes(b'existing output fixture\n')
        shared = work / 'shared-build'
        if source_kind == 'build_existing':
            shared.mkdir()
            (shared / 'build-input').write_bytes(b'existing build fixture\n')
        build_dir.symlink_to(shared, target_is_directory=True)
        original_file = source / 'old-input'
    elif source_kind != 'new':
        output.mkdir()
        source.symlink_to(base if source_kind == 'existing' else directory / 'missing-base', target_is_directory=True)
    shared_before = snapshot(shared) if shared is not None and shared.exists() else {}
    original = snapshot(base)
    before = snapshot(directory)
    paths = {name: {'path': str(path), 'resolved': str(path.resolve()), 'is_symlink': path.is_symlink(),
                    'exists': path.exists(), 'within_work': path.resolve().is_relative_to(work)}
             for name, path in (('work', work), ('out', output), ('source', source), ('build_dir', build_dir))}
    calls, applies, removals, creations, filesystems = [], [], [], [], []
    real_run = kernel.probe_lkl.run
    real_rmtree = kernel.shutil.rmtree
    real_mkdir = Path.mkdir

    def copy(command, **kwargs):
        calls.append({'command': list(map(str, command)), 'timeout': kwargs.get('timeout')})
        if command[:2] != ['/bin/cp', '-cR']:
            raise AssertionError('克隆前段调用了非 cp 命令')
        result = real_run(command, **kwargs)
        calls[-1]['returncode'] = result.returncode
        return result

    def stop(source_path, experiment):
        applies.append({'source': str(source_path), 'experiment': str(experiment)})
        raise BeforePatch()

    def remove(path, *args, **kwargs):
        removals.append(str(path))
        return real_rmtree(path, *args, **kwargs)

    def create(path, *args, **kwargs):
        creations.append(str(path))
        return real_mkdir(path, *args, **kwargs)

    exception = None
    with patch.object(kernel, 'WORK', work), patch.object(kernel, 'OUT', work / 'translation-kernel'), \
            patch.object(kernel.probe_lkl, 'run', copy), patch.object(kernel.probe_lkl, 'apply', stop), \
            patch.object(kernel.shutil, 'rmtree', remove), patch.object(Path, 'mkdir', create), \
            patch.object(kernel.probe_lkl.subprocess, 'check_output', filesystem_reader(filesystems)):
        try:
            kernel.build(guest_page_bytes=page)
        except (Exception, SystemExit) as error:
            exception = {'type': type(error).__name__, 'message': str(error)}
    observed = {'guest_page_bytes': page, 'source_kind': source_kind, 'copy_calls': calls, 'apply_calls': applies,
                'rmtree_calls': removals, 'mkdir_calls': creations, 'paths': paths, 'filesystem_queries': filesystems,
                'exception': exception, 'base_before': original, 'base_after': snapshot(base)}
    if shared is not None:
        shared_after = snapshot(shared) if shared.exists() else {}
        observed.update(shared_before=shared_before, shared_after=shared_after,
                        shared_files_preserved=shared_before == shared_after)
    if original_file is not None:
        observed['original_file_exists'] = original_file.is_file()
    if source_kind != 'new':
        observed.update(passed=exception is not None and exception['type'] == 'RuntimeError' and
                        (expected_error is None or exception['message'] == expected_error) and not calls and
                        not applies and not removals and not creations and snapshot(directory) == before and snapshot(base) == original,
                        case_tree_unchanged=snapshot(directory) == before, expected_exception_message=expected_error)
        return observed
    cloned = snapshot(source) if source.is_dir() and not source.is_symlink() else {}
    separate = bool(cloned) and not source.samefile(base) and all(
        not (source / name).samefile(base / name) for name, entry in original.items() if entry['type'] == 'file')
    content_matches = cloned.keys() == original.keys() and all(
        cloned[name].get('content_hex') == entry.get('content_hex') for name, entry in original.items())
    write_independent = False
    if separate and content_matches:
        (source / 'payload/a').write_bytes(b'clone write ' + str(page).encode() + b'\n')
        (source / 'clone-only').write_bytes(b'clone only\n')
        write_independent = snapshot(base) == original and (source / 'payload/a').read_bytes() != (base / 'payload/a').read_bytes()
    observed.update(clone_before_write=cloned, clone_is_separate=separate, content_matches=content_matches,
                    clone_write_independent=write_independent, base_after=snapshot(base),
                    passed=exception is not None and exception['type'] == 'BeforePatch' and len(calls) == 1 and
                    calls[0]['command'] == ['/bin/cp', '-cR', str(base.resolve()), str(source)] and
                    calls[0].get('returncode') == 0 and len(applies) == 1 and separate and content_matches and write_independent)
    return observed


def consumer_code(module, function, manifest_name, prefix_name):
    path = Path(module.__file__)
    tree = ast.parse(path.read_text())
    entrypoint = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == function)
    assignments = {node.targets[0].id: node for node in entrypoint.body if isinstance(node, ast.Assign) and
                   isinstance(node.targets[0], ast.Name)}
    prefix = assignments[prefix_name]
    identity = entrypoint.body[entrypoint.body.index(prefix) + 1]
    if not isinstance(identity, ast.If) or not any(isinstance(node, ast.Attribute) and node.attr == 'samefile'
                                                  for node in ast.walk(identity)):
        raise ValueError('生产 prefix 身份检查结构变化')
    selected = [assignments[manifest_name], prefix, identity]
    if module is core:
        summary = assignments['input_hashes'].value
    else:
        summary = next(value for key, value in zip(assignments['result'].value.keys, assignments['result'].value.values)
                       if isinstance(key, ast.Constant) and key.value == 'sha256')
    return (compile(ast.Module(body=selected, type_ignores=[]), str(path), 'exec'),
            compile(ast.Expression(summary), str(path), 'eval'),
            {'path': str(path), 'sha256': digest(path),
             'prefix_lines': [[node.lineno, node.end_lineno] for node in selected],
             'summary_lines': [summary.lineno, summary.end_lineno]})


def prefix_case(directory, consumer, variant, kind, compiled):
    directory.mkdir()
    consumer_root, shared = directory / 'consumer-root', directory / 'shared-root'
    work = consumer_root / 'work/native-linux-nce'
    work.mkdir(parents=True)
    shared.mkdir()
    name = ('unicorn-tb-stop-dual' if variant == 'dual' else 'unicorn-tb-stop') if consumer == 'core' else \
           'unicorn-tb-stop-dual' + ('-ios' if variant == 'ios' else '')
    real = shared / name
    real.mkdir()
    alias = work / name
    alias.symlink_to(real, target_is_directory=True)
    local_prefix = alias / 'prefix'
    recorded = real / 'prefix'
    if kind == 'missing_local':
        recorded = shared / 'recorded-prefix'
    (recorded / 'lib').mkdir(parents=True)
    (recorded / 'lib/libunicorn.a').write_bytes(b'host prefix fixture\n')
    if kind == 'different':
        recorded = shared / 'same-bytes-prefix'
        (recorded / 'lib').mkdir(parents=True)
        (recorded / 'lib/libunicorn.a').write_bytes(b'host prefix fixture\n')
    if kind == 'missing_recorded':
        recorded = shared / 'missing-prefix'
    dependency = {'version': '2.1.4', 'code_memory': 'dual' if variant in ('dual', 'macos', 'ios') else 'map-jit',
                  'target': variant if consumer == 'runtime' else 'macos', 'path': str(recorded)}
    (real / 'build.json').write_text(json.dumps(dependency) + '\n')
    before = snapshot(directory)
    code, summary, source = compiled
    context = {'ROOT': consumer_root, 'WORK': work, 'Path': Path, 'dependency': json.loads((real / 'build.json').read_text()),
               'args': SimpleNamespace(code_memory=variant), 'target': variant,
               'parser': argparse.ArgumentParser(prog='shared-prefix-fixture'), 'sha': core.sha, 'digest': runtime.digest}
    stderr, exception, hashes = io.StringIO(), None, None
    try:
        with redirect_stderr(stderr):
            exec(code, context)
        prefix = context['library' if consumer == 'core' else 'unicorn']
        context['files' if consumer == 'core' else 'inputs'] = [alias / 'build.json', prefix / 'lib/libunicorn.a']
        hashes = eval(summary, context)
    except (Exception, SystemExit) as error:
        exception = {'type': type(error).__name__, 'message': str(error)}
    references_local = hashes is not None and all(not Path(name).is_absolute() and '..' not in Path(name).parts and
                                               (consumer_root / name).is_file() for name in hashes)
    selected = context.get('library' if consumer == 'core' else 'unicorn')
    expected_error = 'SystemExit' if consumer == 'core' and kind == 'different' else \
                     'ValueError' if consumer == 'runtime' and kind == 'different' else 'FileNotFoundError'
    unchanged = snapshot(directory) == before
    passed = (exception is None and selected == local_prefix and references_local and
              recorded.is_relative_to(consumer_root) is False) if kind == 'matching' else \
             (exception is not None and exception['type'] == expected_error)
    return {'passed': passed and unchanged, 'consumer': consumer, 'variant': variant, 'kind': kind,
            'consumer_root': str(consumer_root), 'selected_prefix': str(selected), 'recorded_prefix': str(recorded),
            'summary_sha256': hashes, 'summary_references_current_root': references_local,
            'tree_unchanged': unchanged, 'exception': exception, 'stderr': stderr.getvalue(), 'source': source,
            'scope': '只执行生产路径选择、prefix/samefile 和原摘要表达式；dependency 和 files/inputs 为合成小文件引用。'}


def writer_case(directory, code_memory, target, kind):
    directory.mkdir()
    work, shared = directory / 'work', directory / 'shared'
    work.mkdir()
    shared.mkdir()
    source = (work if code_memory == 'map-jit' else work / 'unicorn-dual-source') / 'unicorn-2.1.4'
    output = work / ('unicorn-tb-stop' if code_memory == 'map-jit' else 'unicorn-tb-stop-dual')
    if target == 'ios':
        output = output.with_name(output.name + '-ios')
    archive = work / 'unicorn-2.1.4.tar.gz'
    data = b'host fixture archive\n'
    if kind in ('missing_external_archive', 'shared_archive'):
        target_archive = shared / 'unicorn-2.1.4.tar.gz'
        if kind == 'shared_archive':
            target_archive.write_bytes(data)
        archive.symlink_to(target_archive)
    else:
        archive.write_bytes(data)
    if kind in ('external_source', 'external_output'):
        actual = shared / ('source' if kind == 'external_source' else 'output')
        actual.mkdir()
        (actual / 'existing-file').write_bytes(b'preserved external fixture\n')
        link = source if kind == 'external_source' else output
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(actual, target_is_directory=True)
    before = snapshot(directory)
    tar_calls, downloads, commands = [], [], []

    def stop_archive(path, *args, **kwargs):
        tar_calls.append(str(path))
        raise BeforeArchive()

    def reject_download(*args, **kwargs):
        downloads.append(list(map(str, args)))
        raise AssertionError('fixture 不执行下载')

    def reject_command(*args, **kwargs):
        commands.append(str(args))
        raise AssertionError('fixture 不执行 engine 编译')

    exception = None
    with patch.object(engine, 'WORK', work), patch.object(engine, 'ARCHIVE_SHA', hashlib.sha256(data).hexdigest()), \
            patch.object(engine.tarfile, 'open', stop_archive), patch.object(engine.urllib.request, 'urlretrieve', reject_download), \
            patch.object(engine.subprocess, 'run', reject_command), patch.object(engine.subprocess, 'check_output', reject_command):
        try:
            engine.build(code_memory, target)
        except (Exception, SystemExit) as error:
            exception = {'type': type(error).__name__, 'message': str(error)}
    after = snapshot(directory)
    rejected = kind not in ('local', 'shared_archive')
    existing_unchanged = all(after.get(name) == value for name, value in before.items())
    passed = (exception is not None and exception['type'] == 'RuntimeError' and not tar_calls and after == before) if rejected else \
             (exception is not None and exception['type'] == 'BeforeArchive' and tar_calls == [str(archive)] and
              output.is_dir() and existing_unchanged)
    return {'passed': passed and not downloads and not commands, 'code_memory': code_memory, 'target': target, 'kind': kind,
            'work': str(work), 'exception': exception, 'tar_calls': tar_calls, 'download_calls': downloads,
            'command_calls': commands, 'existing_nodes_unchanged': existing_unchanged, 'tree_unchanged': after == before,
            'before': before, 'after': after, 'archive_sha256_is_fixture': True,
            'fixture_archive_sha256': hashlib.sha256(data).hexdigest(),
            'scope': '实际 build 前段；fixture ARCHIVE_SHA 仅匹配小文件，在 tarfile.open 前停止。'}


def source_fixture(base):
    (base / 'payload/nested').mkdir(parents=True)
    (base / '.droidondarwin-source-revision').write_text(kernel.probe_lkl.REVISION + '\n')
    (base / 'payload/a').write_bytes(b'original fixture\n')
    (base / 'payload/nested/b').write_bytes(b'\0\1\2\n')
    return base


def probe_main_case(directory, other, base, default_base, kind):
    directory.mkdir()
    other.mkdir()
    out = (other if kind == 'base_apfs' else directory) / 'out'
    out.mkdir()
    (out / 'existing-file').write_bytes(b'existing main output fixture\n')
    build = out / 'build'
    source = default_base if kind == 'source_apfs' else base
    argv = []
    expected_error = 'Linux 源码与构建目录需要区分大小写的 APFS 文件系统'
    if kind == 'build_apfs':
        build = other / 'build'
        build.mkdir()
        (build / 'existing-file').write_bytes(b'existing main build fixture\n')
    elif kind in ('process_out_apfs', 'process_build_apfs', 'process_out_cross_fs'):
        argv = ['--processes']
        target = other / 'target'
        target.mkdir()
        (target / 'existing-file').write_bytes(b'existing process fixture\n')
        (out / ('lkl-process-build' if kind == 'process_build_apfs' else 'processes')).symlink_to(target, target_is_directory=True)
        if kind == 'process_out_cross_fs':
            expected_error = 'Linux 源码与构建输出需要位于同一 APFS 文件系统'
    elif kind in ('smp_source_symlink', 'smp_core_source_symlink'):
        argv = ['--processes', '--smp']
        variant = 'smp'
        if kind == 'smp_core_source_symlink':
            argv.append('--core-scheduling')
            variant = 'smp-core'
        (out / f'lkl-{variant}-process-source').symlink_to(base, target_is_directory=True)
        expected_error = 'SMP 源码克隆需要同一 APFS 文件系统内的独立目录'
    before, other_before = snapshot(directory), snapshot(other)
    inputs = {'source': snapshot(base), 'default_source': snapshot(default_base)}
    calls = {name: [] for name in ('mkdir', 'rmtree', 'run', 'copyfile', 'apply')}
    filesystems, exception = [], None

    def reject(operation, *values, **kwargs):
        calls[operation].append(list(map(str, values)))
        raise AssertionError('拒绝负例到达写入调用：' + operation)

    probe = kernel.probe_lkl
    with patch.object(probe, 'OUT', out), patch.object(probe, 'BUILD', build), patch.object(probe, 'SOURCE', source), \
            patch.object(probe, 'PROCESS', False, create=True), patch.object(probe, 'SMP', False), \
            patch.object(sys, 'argv', ['probe_lkl.py', *argv]), \
            patch.object(Path, 'mkdir', lambda *values, **kwargs: reject('mkdir', *values, **kwargs)), \
            patch.object(probe.shutil, 'rmtree', lambda *values, **kwargs: reject('rmtree', *values, **kwargs)), \
            patch.object(probe.shutil, 'copyfile', lambda *values, **kwargs: reject('copyfile', *values, **kwargs)), \
            patch.object(probe, 'run', lambda *values, **kwargs: reject('run', *values, **kwargs)), \
            patch.object(probe, 'apply', lambda *values, **kwargs: reject('apply', *values, **kwargs)), \
            patch.object(probe.subprocess, 'check_output', filesystem_reader(filesystems)):
        try:
            probe.main()
        except (Exception, SystemExit) as error:
            exception = {'type': type(error).__name__, 'message': str(error)}
    after, other_after = snapshot(directory), snapshot(other)
    inputs_after = {'source': snapshot(base), 'default_source': snapshot(default_base)}
    return {'passed': exception is not None and exception['type'] == 'RuntimeError' and
            exception['message'] == expected_error and not any(calls.values()) and before == after and
            other_before == other_after and inputs == inputs_after,
            'kind': kind, 'argv': argv, 'initial_paths': {'SOURCE': str(source), 'OUT': str(out), 'BUILD': str(build)},
            'exception': exception, 'expected_exception_message': expected_error, 'write_calls': calls,
            'filesystem_queries': filesystems, 'before': before, 'after': after, 'other_before': other_before,
            'other_after': other_after, 'inputs_unchanged': inputs == inputs_after,
            'scope': '实际 probe_lkl.main 目录选择与写入前拒绝；任何写入调用均记录后停止。'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture-directory', type=Path)
    parser.add_argument('--other-filesystem-directory', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    fixture_parent = (args.fixture_directory or output.parent).resolve(strict=True)
    sources = {name: digest(ROOT / name) for name in FROZEN}
    sources[str(Path(__file__).relative_to(ROOT))] = digest(Path(__file__))
    cases = [{'case': 'frozen_sources', 'passed': all(sources[name] == expected for name, expected in FROZEN.items()),
              'expected': FROZEN, 'observed': {name: sources[name] for name in FROZEN}}]
    temporary = None
    additional_roots = []
    if cases[0]['passed']:
        with ExitStack() as fixtures:
            temporary = Path(fixtures.enter_context(tempfile.TemporaryDirectory(prefix='shared-inputs-', dir=fixture_parent)))
            base = source_fixture(temporary / 'source-base')
            for page in (4096, 16384):
                for kind in ('new', 'existing', 'dangling', 'ancestor', 'out_internal', 'external_work', 'build_existing', 'build_dangling'):
                    name = f'kernel/{page}/{kind}'
                    record(cases, name, lambda page=page, kind=kind: clone_case(temporary / name.replace('/', '-'), base, page, kind))
            for consumer, module, function, manifest_name, prefix_name, variants in (
                    ('core', core, 'main', 'manifest', 'library', ('map-jit', 'dual')),
                    ('runtime', runtime, 'build', 'dependency_path', 'unicorn', ('macos', 'ios'))):
                compiled = consumer_code(module, function, manifest_name, prefix_name)
                for variant in variants:
                    for kind in ('matching', 'different', 'missing_local', 'missing_recorded'):
                        name = f'{consumer}/{variant}/{kind}'
                        record(cases, name, lambda consumer=consumer, variant=variant, kind=kind, compiled=compiled:
                               prefix_case(temporary / name.replace('/', '-'), consumer, variant, kind, compiled))
            for code_memory, target in (('map-jit', 'macos'), ('dual', 'macos'), ('dual', 'ios')):
                for kind in ('local', 'shared_archive', 'external_source', 'external_output', 'missing_external_archive'):
                    name = f'writer/{code_memory}/{target}/{kind}'
                    record(cases, name, lambda code_memory=code_memory, target=target, kind=kind:
                           writer_case(temporary / name.replace('/', '-'), code_memory, target, kind))
            if args.other_filesystem_directory:
                ordinary = Path(fixtures.enter_context(tempfile.TemporaryDirectory(prefix='shared-inputs-apfs-', dir=output.parent)))
                secondary = Path(fixtures.enter_context(tempfile.TemporaryDirectory(
                    prefix='shared-inputs-other-', dir=args.other_filesystem_directory.resolve(strict=True))))
                additional_roots = [ordinary, secondary]
                default_base = source_fixture(ordinary / 'source-base')
                for page in (4096, 16384):
                    for kind, directory, source, expected in (
                            ('default_apfs_input', temporary, default_base, 'Linux 源码与构建目录需要区分大小写的 APFS 文件系统'),
                            ('default_apfs_output', ordinary, base, 'Linux 源码与构建目录需要区分大小写的 APFS 文件系统'),
                            ('cross_filesystem', secondary, base, 'APFS 克隆需要 Linux 源码与构建目录位于同一文件系统')):
                        name = f'kernel/{page}/{kind}'
                        record(cases, name, lambda directory=directory, source=source, page=page, name=name, expected=expected:
                               clone_case(directory / name.replace('/', '-'), source, page, 'filesystem', expected))
                for kind in ('source_apfs', 'base_apfs', 'build_apfs', 'process_out_apfs', 'process_build_apfs',
                             'smp_source_symlink', 'smp_core_source_symlink', 'process_out_cross_fs'):
                    name = 'probe_main/' + kind
                    other = secondary if kind == 'process_out_cross_fs' else ordinary
                    record(cases, name, lambda kind=kind, name=name, other=other:
                           probe_main_case(temporary / name.replace('/', '-'), other / name.replace('/', '-'), base, default_base, kind))
    source_unchanged = all(digest(ROOT / name) == value for name, value in sources.items())
    cleaned = (temporary is None or not temporary.exists()) and all(not path.exists() for path in additional_roots)
    result = {'test': 'translation_shared_inputs', 'passed': all(case['passed'] for case in cases) and source_unchanged and cleaned,
              'platform': platform.platform(), 'cases': cases, 'source_sha256': sources, 'source_unchanged': source_unchanged,
              'cleanup': {'temporary_root': str(temporary), 'additional_roots': list(map(str, additional_roots)), 'removed': cleaned},
              'kernel_built': False, 'engine_built': False, 'guest_executed': False,
              'scope': '实际 cp 克隆与真实 build 前段；prefix 和摘要为生产 AST，小文件、依赖报告、消费者 ROOT 与 writer archive SHA 为 fixture。'}
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': len(cases),
                      'failed': [case['case'] for case in cases if not case['passed']]}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
