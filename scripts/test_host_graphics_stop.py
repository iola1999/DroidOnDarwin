#!/usr/bin/env python3
"""检查真实 renderer 的连接停止，以及受控满 channel 和创建竞态。"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys

from probe_lkl_translation import build_graphics
from process_observer import observe

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
HOST = EXP / 'graphics/host-graphics.cpp'
FIXTURE = HOST.with_name('host-graphics-stop-test.cpp')
CASES = ('full-channel', 'creation-during-stop', 'raw-flags-stop',
         'incomplete-handshake', 'real-idle-channel', 'zero-connections')


def sha(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def connection(text):
    start, end = 'struct Connection {', '\n};\n\nint nce_graphics_start('
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError('生产 Connection 的提取位置变化')
    return text[text.index(start):text.index(end) + len('\n};')] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--graphics-prefix', type=Path)
    parser.add_argument('--graphics-angle', type=Path)
    parser.add_argument('--baseline-ref', help='可选的旧 commit，用于有界复现满 channel 停止等待')
    args = parser.parse_args()
    common = Path(subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT,
                                         text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    main_root = common.resolve().parent
    out = args.output.resolve()
    if not any(out.is_relative_to(base / 'work') for base in (ROOT, main_root)):
        parser.error('结果目录需要位于项目 work 下')
    prefix = (args.graphics_prefix or main_root / 'work/gfxstream-build-macos-ios-api/prefix').resolve(strict=True)
    angle = (args.graphics_angle or main_root / 'work/angle-macos/prefix/lib').resolve(strict=True)
    out.mkdir(parents=True, exist_ok=False)
    report_path = out / 'result.json'
    report = {'status': 'running', 'scope': '宿主 Connection 停止与 renderer 生命周期；无 guest 运行',
              'platform': platform.platform(), 'command': [sys.executable, *sys.argv],
              'inputs': {}, 'commands': [], 'runs': [],
              'limitations': ['listener 与 accept 使用 Linux syscall fixture，连接 I/O 使用真实 macOS socketpair',
                              '满 channel、创建停留和 raw flags 竞态使用受控 RenderChannel，实际 Connection 从生产文件提取',
                              '真实 gfxstream/ANGLE 覆盖零连接、半握手及空闲 opengles channel；未验证图形命令、像素、LKL 权限或完整启动']}
    artifacts = []

    def save():
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')

    def command(argv, label):
        argv = list(map(str, argv))
        item = {'command': argv, 'stdout': str(out / (label + '.stdout')),
                'stderr': str(out / (label + '.stderr')), 'timeout_seconds': 120}
        report['commands'].append(item)
        save()
        with Path(item['stdout']).open('xb') as output, Path(item['stderr']).open('xb') as error:
            completed = subprocess.run(argv, cwd=ROOT, stdout=output, stderr=error, timeout=120)
        item.update(returncode=completed.returncode, stdout_sha256=sha(item['stdout']), stderr_sha256=sha(item['stderr']))
        save()
        if completed.returncode:
            raise ValueError('命令失败，日志见 ' + item['stderr'])

    def run(binary, entry, case, *, baseline=False):
        label = ('baseline-' if baseline else '') + entry + '-' + case
        item = {'entry': entry, 'case': case, 'baseline': baseline,
                'command': [str(binary), entry, case], 'timeout_seconds': 3 if baseline else 20,
                'stdout': str(out / (label + '.jsonl')), 'stderr': str(out / (label + '.log'))}
        report['runs'].append(item)
        save()
        environment = {**os.environ, 'DYLD_LIBRARY_PATH': str(angle), 'ANGLE_DEFAULT_PLATFORM': 'metal'}
        with Path(item['stdout']).open('xb') as output, Path(item['stderr']).open('xb') as error:
            host = observe(item['command'], environment, output, error, item['timeout_seconds'])
        rows = [json.loads(line) for line in Path(item['stdout']).read_text().splitlines() if line.startswith('{')]
        item.update(host_process=host, results=rows, stdout_sha256=sha(item['stdout']), stderr_sha256=sha(item['stderr']))
        if baseline:
            item['validated'] = (host['timed_out'] and host['process_group_empty'] and
                                 {'phase': 'controlled_channel_waits', 'write': True, 'read': True} in rows)
        else:
            expected = {'test': 'host_graphics_stop', 'entry': entry, 'case': case, 'passed': True}
            item['validated'] = (host['returncode'] == 0 and not host['timed_out'] and host['process_group_empty'] and
                                 not host['errors'] and not host['cleanup_signals'] and expected in rows)
            channels = [row for row in rows if row.get('test') == 'native_graphics_channels_released']
            if case in ('incomplete-handshake', 'real-idle-channel', 'zero-connections'):
                count, cycles = (0, 2) if case == 'zero-connections' else (1, 1)
                item['validated'] &= channels == [{'test': 'native_graphics_channels_released', 'passed': True,
                                                   'opened': count, 'closed': count}] * cycles
        save()
        if not item['validated']:
            raise ValueError('停止回归失败，日志见 ' + item['stdout'])

    save()
    try:
        paths = [HOST, FIXTURE, HOST.with_name('graphics-socket-address.h'), Path(__file__).resolve(),
                 ROOT / 'scripts/probe_lkl_translation.py', ROOT / 'scripts/process_observer.py']
        report['inputs'] = {str(path): sha(path) for path in paths}
        command(['bash', 'scripts/format.sh', '--check', HOST, FIXTURE], 'format')
        versions = [('fixed', HOST.read_text())]
        if args.baseline_ref:
            head = subprocess.check_output(['git', 'rev-parse', '--verify', '--end-of-options', args.baseline_ref + '^{commit}'],
                                           cwd=ROOT, text=True, timeout=10).strip()
            previous = subprocess.check_output(['git', 'show', head + ':experiments/native-linux/graphics/host-graphics.cpp'],
                                               cwd=ROOT, text=True, timeout=10)
            report['baseline'] = {'commit': head, 'connection_sha256': hashlib.sha256(connection(previous).encode()).hexdigest()}
            versions.insert(0, ('baseline', previous))
        artifacts += [out / 'host-graphics.o', out / 'host-graphics.d']
        build, link, files = build_graphics(out, prefix=prefix, angle=angle)
        report['commands'].append({'command': build, 'result': 'passed'})
        report['inputs'].update({str(path): sha(path) for path in files})
        report['compiled_outputs'] = {str(out / 'host-graphics.o'): {'sha256': sha(out / 'host-graphics.o'),
                                                                   'bytes': (out / 'host-graphics.o').stat().st_size}}
        binaries = {}
        for name, text in versions:
            directory = out / name
            directory.mkdir()
            header = directory / 'connection-under-test.h'
            header.write_text('/* SPDX-License-Identifier: GPL-2.0-or-later */\n' + connection(text))
            binary, dependency = out / (name + '-stop'), out / (name + '.d')
            artifacts += [header, binary, dependency, binary.with_name(binary.name + '.dSYM')]
            argv = ['xcrun', 'clang++', '-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-return-type-c-linkage', '-I', EXP / 'graphics', '-iquote', directory,
                    '-isystem', ROOT / 'work/gfxstream-src/host/include',
                    '-isystem', ROOT / 'work/gfxstream-src/host/features/include',
                    '-isystem', ROOT / 'work/gfxstream-src/common/base/include',
                    '-MD', '-MF', dependency, FIXTURE, *link, '-o', binary]
            command(argv, name + '-compile')
            report.setdefault('generated_inputs', {})[str(header)] = sha(header)
            report.setdefault('compiled_outputs', {})[str(binary)] = {'sha256': sha(binary), 'bytes': binary.stat().st_size}
            dependencies = shlex.split(dependency.read_text().replace(chr(92) + '\n', ' ').split(':', 1)[1])
            for path in map(Path, dependencies):
                if path.is_relative_to(ROOT / 'work/gfxstream-src'):
                    report['inputs'][str(path)] = sha(path)
            binaries[name] = binary
        for entry in ('cli', 'gcd'):
            if 'baseline' in binaries:
                run(binaries['baseline'], entry, 'full-channel', baseline=True)
            for case in CASES:
                run(binaries['fixed'], entry, case)
        report['inputs_unchanged'] = all(sha(path) == expected for path, expected in report['inputs'].items())
        if not report['inputs_unchanged']:
            raise ValueError('回归期间输入发生变化')
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error={'type': type(error).__name__, 'message': str(error)})
        raise
    finally:
        removed, removed_directories = [], []
        for path in artifacts:
            if path.is_file():
                removed.append({'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)})
                path.unlink()
            elif path.is_dir():
                for child in sorted(path.rglob('*')):
                    if child.is_file():
                        removed.append({'path': str(child), 'bytes': child.stat().st_size, 'sha256': sha(child)})
                shutil.rmtree(path)
                removed_directories.append(str(path))
        for name in ('baseline', 'fixed'):
            directory = out / name
            if directory.is_dir() and not any(directory.iterdir()):
                directory.rmdir()
                removed_directories.append(str(directory))
        cleanup = main_root / ('work/cleanup-' + datetime.now().strftime('%Y-%m-%d'))
        cleanup.mkdir(exist_ok=True)
        cleanup_path = cleanup / (out.name + '.json')
        if cleanup_path.exists():
            raise ValueError('清理记录已存在：' + str(cleanup_path))
        cleanup_path.write_text(json.dumps({'removed': removed, 'removed_directories': removed_directories,
                                          'result': str(report_path)}, ensure_ascii=False, indent=2) + '\n')
        report['cleanup'] = {'path': str(cleanup_path), 'sha256': sha(cleanup_path)}
        save()
    print(json.dumps({'status': report['status'], 'report': str(report_path), 'runs': len(report['runs'])}, ensure_ascii=False))


if __name__ == '__main__':
    main()
