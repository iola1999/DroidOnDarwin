#!/usr/bin/env python3
"""从已验证的双映射构建编译 macOS/iOS 翻译运行库。设备运行仍需独立验收。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shlex
import subprocess

from build_translation_engine import build as build_unicorn
from probe_direct_execution import build_engine

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
WORK = ROOT / 'work/native-linux-nce'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(report_path, out, target, *, reuse_engine_build=False):
    report_path, out = report_path.resolve(), out.resolve()
    if not out.is_relative_to(WORK) or target not in ('macos', 'ios'):
        raise ValueError('需要 macos/ios 目标，输出必须在 work/native-linux-nce')
    report = json.loads(report_path.read_text())
    config = report['configuration']
    guest_page_bytes = config.get('guest_page_bytes', 16384)
    if config['execution'] != 'direct' or config['code_memory'] != 'dual' or not report['runs'] or not all(
            run['validated'] for run in report['runs']):
        raise ValueError('需要已成功验证的 Direct 双映射构建')
    source_hashes = {report_path: digest(report_path), Path(__file__): digest(Path(__file__))}
    for name, expected in report['sha256'].items():
        path = ROOT / name
        if not path.is_file() or digest(path) != expected:
            raise ValueError('本机验证输入已变化：' + name)
        source_hashes[path] = expected
    kernel_images = [ROOT / name for name in report['sha256'] if name.endswith('/lkl-image.h')]
    if len(kernel_images) != 1:
        raise ValueError('本机验证记录需要唯一的内核镜像头')
    kernel = kernel_images[0].parent
    kernel_build_path = kernel / 'build.json'
    if str(kernel_build_path.relative_to(ROOT)) not in report['sha256']:
        raise ValueError('内核构建记录未纳入本机验证输入')
    kernel_build = json.loads(kernel_build_path.read_text())
    configuration_keys = ('cpus', 'guest_page_bytes', 'host_page_bytes', 'high_res_timers', 'bpf_jit')
    if any(name not in config or name not in kernel_build for name in configuration_keys):
        raise ValueError('内核构建与本机验证记录需要完整的 CPU、页大小、定时器和 BPF 配置')
    kernel_configuration = {name: config[name] for name in configuration_keys}
    for name, expected in kernel_configuration.items():
        value_type = bool if name in ('high_res_timers', 'bpf_jit') else int
        if type(expected) is not value_type or type(kernel_build[name]) is not value_type or kernel_build[name] != expected:
            raise ValueError('内核构建配置与本机验证记录不一致：' + name)
    if not kernel_build.get('sha256'):
        raise ValueError('内核构建记录缺少实际来源摘要')
    for name, expected in kernel_build['sha256'].items():
        path = ROOT / name
        if not path.is_file() or digest(path) != expected or source_hashes.get(path, expected) != expected:
            raise ValueError('内核构建输入已变化：' + name)
        source_hashes[path] = expected
    fixtures = report_path.parent
    dependency_path = WORK / ('unicorn-tb-stop-dual' + ('-ios' if target == 'ios' else '')) / 'build.json'
    dependency = json.loads(dependency_path.read_text()) if dependency_path.is_file() else {}
    if (dependency.get('target') != target or dependency.get('code_memory') != 'dual' or
            not dependency.get('sha256') or any(not (ROOT / name).is_file() or digest(ROOT / name) != value
                                               for name, value in dependency['sha256'].items())):
        if reuse_engine_build:
            raise ValueError('Unicorn 构建输入或产物已变化，需要先核对共享构建')
        dependency = build_unicorn('dual', target)
    unicorn = dependency_path.parent / 'prefix'
    if not unicorn.samefile(Path(dependency['path'])):
        raise ValueError('Unicorn 库目录与构建记录不一致')
    source_hashes[dependency_path] = digest(dependency_path)
    for name, expected in dependency['sha256'].items():
        path = ROOT / name
        if source_hashes.get(path, expected) != expected:
            raise ValueError('Unicorn 来源与本机验证记录不一致：' + name)
        source_hashes[path] = expected
    out.mkdir(parents=True, exist_ok=True)
    objects, inputs = [], []
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
    header_dependencies = {}
    sources = ['translation/kernel-image.c', 'translation/linux-elf.c', 'translation/linux-execution.c', 'translation/interaction-profile.c',
               'translation/linux-runner.c', 'translation/linux-memory.c', 'translation/code-watch.c',
               'translation/read-view.c', 'translation/unicorn-code-memory.c', 'lkl-host.c', 'input.c',
               'session.c', 'session-profile.c', 'graphics/host-graphics.cpp']
    if config.get('bpf_jit'):
        sources.append('translation/bpf-code-memory.c')
    for name in sources:
        source = EXP / name
        if not source.is_file():
            raise ValueError('运行库源码不存在：' + name)
        expected = digest(source)
        if source_hashes.get(source, expected) != expected:
            raise ValueError('运行库源码已变化：' + name)
        source_hashes[source] = expected
        obj = out / (source.stem + '.o')
        dependency_file = obj.with_suffix('.d')
        pre_dependency_file = obj.with_suffix('.pre.d')
        extra = []
        if source.suffix == '.cpp':
            extra = ['-std=c++17', '-Wno-return-type-c-linkage']
            for directory in ('host/include', 'host/features/include', 'common/base/include'):
                extra += ['-isystem', str(ROOT / 'work/gfxstream-src' / directory)]
        command = ['xcrun', 'clang++' if source.suffix == '.cpp' else 'clang', *flags, *extra,
                   '-M', '-MF', str(pre_dependency_file), '-MT', str(obj), str(source)]
        subprocess.run(command, check=True, timeout=120)
        commands.append(command)
        headers = shlex.split(pre_dependency_file.read_text().replace('\\\n', ' ').split(':', 1)[1])
        header_paths = [Path(name) for name in headers if Path(name).is_relative_to(ROOT) and Path(name) != source]
        for path in header_paths:
            if not path.is_file():
                raise ValueError('运行库头文件不存在：' + str(path.relative_to(ROOT)))
            expected = digest(path)
            if source_hashes.get(path, expected) != expected:
                raise ValueError('运行库头文件已变化：' + str(path.relative_to(ROOT)))
            source_hashes[path] = expected
        command = ['xcrun', 'clang++' if source.suffix == '.cpp' else 'clang', *flags, *extra,
                   '-MD', '-MF', str(dependency_file), '-c', str(source), '-o', str(obj)]
        subprocess.run(command, check=True, timeout=120)
        commands.append(command)
        objects.append(obj)
        inputs.append(source)
        headers = shlex.split(dependency_file.read_text().replace('\\\n', ' ').split(':', 1)[1])
        actual_header_paths = [Path(name) for name in headers if Path(name).is_relative_to(ROOT) and Path(name) != source]
        if set(actual_header_paths) != set(header_paths):
            raise ValueError('运行库编译与预处理的头文件依赖不一致：' + name)
        inputs += header_paths
        header_dependencies[str(source.relative_to(ROOT))] = [str(path.relative_to(ROOT)) for path in header_paths]
    direct_objects, direct_inputs = build_engine(out / 'direct', 'dual', target, guest_page_bytes=guest_page_bytes)
    for path in direct_inputs:
        if path not in source_hashes or not path.is_file() or digest(path) != source_hashes[path]:
            raise ValueError('Direct 编译输入未通过本机来源复核：' + str(path.relative_to(ROOT)))
    objects = direct_objects + objects
    inputs += direct_inputs
    library = out / 'libtranslation.a'
    command = ['xcrun', 'libtool', '-static', '-o', str(library), *map(str, objects), str(unicorn / 'lib/libunicorn.a')]
    subprocess.run(command, check=True, timeout=120)
    commands.append(command)
    for path, expected in source_hashes.items():
        if not path.is_file() or digest(path) != expected:
            raise ValueError('运行库编译期间输入已变化：' + str(path.relative_to(ROOT)))
    inputs += [Path(__file__), report_path, dependency_path, *[ROOT / name for name in dependency['sha256']],
               *source_hashes,
               kernel / 'lkl-image.h', kernel / 'lkl-host-ops.h', fixtures / 'translation-symbols.h',
               *sorted((EXP / 'translation').glob('*.h')), *sorted((WORK / 'processes/host-include').glob('*.h')),
               EXP / 'lkl-host-ops.h', EXP / 'layout.h', EXP / 'session.h', EXP / 'session-internal.h', EXP / 'interaction-profile.h']
    inputs = sorted(set(path for path in inputs if path.is_file()))
    result = {'scope': 'Cross compilation; device execution pending', 'target': target, 'sdk': sdk,
              'configuration': config, 'source_report': str(report_path.relative_to(ROOT)),
              'kernel_build': str(kernel_build_path.relative_to(ROOT)), 'kernel_configuration': kernel_configuration,
              'source_inputs_unchanged_after_build': True, 'header_dependencies': header_dependencies,
              'commands': commands, 'library': str(library), 'library_sha256': digest(library),
              'sha256': {str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path): digest(path) for path in inputs}}
    (out / 'build.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe-report', type=Path, required=True)
    parser.add_argument('--target', choices=('macos', 'ios'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reuse-engine-build', action='store_true')
    args = parser.parse_args()
    print(build(args.probe_report, args.output, args.target, reuse_engine_build=args.reuse_engine_build)['library'])
