#!/usr/bin/env python3
"""从已验证的双映射构建编译 macOS/iOS 翻译运行库。设备运行仍需独立验收。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

from build_translation_engine import build as build_unicorn
from probe_direct_execution import build_engine

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
WORK = ROOT / 'work/native-linux-nce'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(report_path, out, target):
    report_path, out = report_path.resolve(), out.resolve()
    if not out.is_relative_to(WORK) or target not in ('macos', 'ios'):
        raise ValueError('需要 macos/ios 目标，输出必须在 work/native-linux-nce')
    report = json.loads(report_path.read_text())
    config = report['configuration']
    guest_page_bytes = config.get('guest_page_bytes', 16384)
    if config['execution'] != 'direct' or config['code_memory'] != 'dual' or not report['runs'] or not all(
            run['validated'] for run in report['runs']):
        raise ValueError('需要已成功验证的 Direct 双映射构建')
    for name, expected in report['sha256'].items():
        if digest(ROOT / name) != expected:
            raise ValueError('本机验证输入已变化：' + name)
    kernel = next((ROOT / name).parent for name in report['sha256'] if name.endswith('/lkl-image.h'))
    fixtures = report_path.parent
    dependency_path = WORK / ('unicorn-tb-stop-dual' + ('-ios' if target == 'ios' else '')) / 'build.json'
    dependency = json.loads(dependency_path.read_text()) if dependency_path.is_file() else {}
    if (dependency.get('target') != target or dependency.get('code_memory') != 'dual' or
            not dependency.get('sha256') or any(not (ROOT / name).is_file() or digest(ROOT / name) != value
                                               for name, value in dependency['sha256'].items())):
        dependency = build_unicorn('dual', target)
    unicorn = dependency_path.parent / 'prefix'
    if not unicorn.samefile(Path(dependency['path'])):
        raise ValueError('Unicorn 库目录与构建记录不一致')
    objects, inputs = build_engine(out / 'direct', 'dual', target, guest_page_bytes=guest_page_bytes)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos' if target == 'ios' else 'macosx', '--show-sdk-path'], text=True).strip()
    flags = ['-target', 'arm64-apple-ios26.0' if target == 'ios' else 'arm64-apple-macos' + platform.mac_ver()[0],
             '-isysroot', sdk, '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-DNCE_IOS_APP', '-DNCE_PROCESS',
             f'-DNCE_TRANSLATION_CPUS={config["cpus"]}', f'-DNCE_GUEST_PAGE_SHIFT={guest_page_bytes.bit_length() - 1}',
             '-DNCE_TRANSLATION_DUAL_MAP', '-DNCE_DIRECT_EXECUTION', '-DNCE_GRAPHICS']
    if config['cpus'] == 2:
        flags.append('-DNCE_SMP')
    for include in (EXP, kernel, fixtures, WORK / 'processes/host-include', unicorn / 'include', ROOT / 'src/ios-jit'):
        flags += ['-I', str(include)]
    commands = []
    sources = ['translation/kernel-image.c', 'translation/linux-elf.c', 'translation/linux-execution.c', 'translation/interaction-profile.c',
               'translation/linux-runner.c', 'translation/linux-memory.c', 'translation/code-watch.c',
               'translation/read-view.c', 'translation/unicorn-code-memory.c', 'lkl-host.c', 'input.c',
               'session.c', 'session-profile.c', 'graphics/host-graphics.cpp']
    for name in sources:
        source = EXP / name
        obj = out / (source.stem + '.o')
        extra = []
        if source.suffix == '.cpp':
            extra = ['-std=c++17', '-Wno-return-type-c-linkage']
            for directory in ('host/include', 'host/features/include', 'common/base/include'):
                extra += ['-isystem', str(ROOT / 'work/gfxstream-src' / directory)]
        command = ['xcrun', 'clang++' if source.suffix == '.cpp' else 'clang', *flags, *extra, '-c', str(source), '-o', str(obj)]
        subprocess.run(command, check=True, timeout=120)
        commands.append(command)
        objects.append(obj)
        inputs.append(source)
    library = out / 'libtranslation.a'
    command = ['xcrun', 'libtool', '-static', '-o', str(library), *map(str, objects), str(unicorn / 'lib/libunicorn.a')]
    subprocess.run(command, check=True, timeout=120)
    commands.append(command)
    inputs += [Path(__file__), report_path, dependency_path, *[ROOT / name for name in dependency['sha256']],
               kernel / 'lkl-image.h', kernel / 'lkl-host-ops.h', fixtures / 'translation-symbols.h',
               *sorted((EXP / 'translation').glob('*.h')), *sorted((WORK / 'processes/host-include').glob('*.h')),
               EXP / 'lkl-host-ops.h', EXP / 'layout.h', EXP / 'session.h', EXP / 'session-internal.h', EXP / 'interaction-profile.h']
    inputs = sorted(set(path for path in inputs if path.is_file()))
    result = {'scope': 'Cross compilation; device execution pending', 'target': target, 'sdk': sdk,
              'configuration': config, 'source_report': str(report_path.relative_to(ROOT)),
              'commands': commands, 'library': str(library), 'library_sha256': digest(library),
              'sha256': {str(path.relative_to(ROOT)): digest(path) for path in inputs}}
    (out / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe-report', type=Path, required=True)
    parser.add_argument('--target', choices=('macos', 'ios'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(build(args.probe_report, args.output, args.target)['library'])
