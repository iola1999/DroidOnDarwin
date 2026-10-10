#!/usr/bin/env python3
"""将已通过桌面回归的 LKL/NCE 构建打包为独立 macOS 应用。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
INPUT = ROOT / 'work/native-linux-nce/processes'
OUT = ROOT / 'work/native-linux-nce/macos'


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=kwargs.pop('timeout', 600), **kwargs)


def digest(path):
    with Path(path).open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def validate_inputs():
    summary = json.loads((INPUT / 'lkl-summary.json').read_text())
    files = {ROOT / name: value for name, value in summary['input_sha256'].items()}
    files.update({INPUT / name: value for name, value in summary['generated_sha256'].items()})
    files[INPUT / 'lkl-image.bin'] = summary['kernel_sha256']
    files.update({Path(name): value for name, value in summary['graphics_libraries_sha256'].items()})
    for path, expected in files.items():
        if digest(path) != expected:
            raise SystemExit(f'输入发生变化，请重新运行桌面回归：{path}')
    for mode in ('results', 'dispatch_results', 'window_results'):
        records = summary.get(mode, [])
        required = {'android_system_boot_completed', 'android_clock_controls_ready',
                    'android_application_data_persistence', 'android_keystore_persistence',
                    'android_system_process_groups_stopped', 'linux_data_readonly_shutdown'}
        passed = {r.get('test') for r in records if r.get('passed')}
        if required - passed or not records or records[-1].get('result') != 'passed':
            raise SystemExit('缺少成功的桌面回归：' + mode)
        if not any(r.get('phase') == 'linux_mapping_policy' and r.get('mode') == 'demand' for r in records):
            raise SystemExit('缺少 demand 映射检查：' + mode)
    return files


def write_notices(resources):
    android = ROOT / 'work/native-linux-nce/android'
    paths = [ROOT / 'LICENSE', ROOT / 'work/native-linux-nce/lkl-source/COPYING',
             ROOT / 'work/native-linux-nce/musl-1.2.5/COPYRIGHT',
             ROOT / 'work/native-linux-nce/dash-0.5.12/COPYING',
             ROOT / 'work/native-linux-nce/compiler-rt-20.1.8.src/LICENSE.TXT']
    paths += [android / name for name in [
        'android36/NOTICE.txt', 'android36-aosp-components/NOTICE.txt', 'art/NOTICE',
        'bionic-source/libc/NOTICE', 'bionic-source/linker/NOTICE', 'bionic-source/libdl/NOTICE',
        'scudo/LICENSE.TXT', 'libunwind-20.1.8.src/LICENSE.TXT', 'fmt/LICENSE', 'dlmalloc/NOTICE',
        'libnativehelper/NOTICE', 'cpu_features/LICENSE', 'tinyxml2/LICENSE.txt', 'lz4/lib/LICENSE',
        'lzma/NOTICE', 'libbase/NOTICE', 'liblog/liblog/NOTICE', 'cronet-boringssl/src/LICENSE',
        'vixl/LICENCE', 'boringssl/src/LICENSE']]
    gfx = ROOT / 'work/gfxstream-src'
    angle = ROOT / 'third_party/gpu/webkit/Source/ThirdParty/ANGLE'
    paths += [gfx / 'LICENSE', *sorted((gfx / 'third_party').rglob('LICENSE*'))]
    paths += sorted(p for p in angle.rglob('*') if p.is_file() and
                    p.name.upper().startswith(('LICENSE', 'COPYING')) and '.xcarchive/' not in str(p))
    with (resources / 'ThirdPartyNotices.txt').open('wb') as notices:
        for path in paths:
            if path.is_file():
                notices.write(f'\n===== {path.relative_to(ROOT)} =====\n'.encode())
                notices.write(path.read_bytes())
    shutil.copyfile(android / 'android36/etc/NOTICE.xml.gz', resources / 'NOTICE.xml.gz')


def audit_libraries(path):
    dependencies = subprocess.check_output(['otool', '-L', path], text=True).splitlines()[1:]
    for line in dependencies:
        library = line.strip().split(' (')[0]
        if not library.startswith(('/System/Library/', '/usr/lib/', '@rpath/')):
            raise SystemExit(f'包内文件仍依赖外部动态库：{path}: {library}')
        if library.startswith('@rpath/') and not (path.parent.parent / 'Frameworks' / library[7:]).is_file():
            raise SystemExit(f'缺少包内动态库：{library}')
    commands = subprocess.check_output(['otool', '-l', path], text=True)
    rpaths = re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', commands)
    if any(entry not in ('@loader_path', '@executable_path') and
           not entry.startswith(('@loader_path/', '@executable_path/')) for entry in rpaths):
        raise SystemExit(f'包内文件仍含外部 rpath：{path}: {rpaths}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rebuild-kernel', action='store_true')
    parser.add_argument('--replace', action='store_true', help='替换未运行的本机生成包，保留用户数据')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise SystemExit('需要 Apple Silicon Mac')
    if args.rebuild_kernel:
        run(['python3', ROOT / 'scripts/probe_lkl.py', '--processes', '--graphics-angle',
             ROOT / 'work/angle-macos/prefix/lib', '--graphics-window'], timeout=1800,
            env={**os.environ, 'NCE_MM_MODE': 'demand', 'NCE_SYSTEM_SERVER_PROBE': '1'})
    verified = validate_inputs()
    local = [*[p for p in sorted((EXP / 'macos').glob('*')) if p.suffix in ('.h', '.mm', '.swift') and not p.name.endswith('Tests.mm')],
             *[p for p in sorted((EXP / "app").glob("*")) if p.suffix in (".h", ".mm", ".swift") and not p.name.endswith("Tests.mm")], ROOT / "scripts/package_native_linux_image.py", EXP / 'ios/Bridge.c', EXP / 'ios/Bridge.h', Path(__file__).resolve()]
    verified.update({p: digest(p) for p in local if p.is_file()})
    OUT.mkdir(parents=True, exist_ok=True)
    app = OUT / 'Husk NCE.app'
    if app.exists():
        if not args.replace:
            raise SystemExit(f'输出已存在，重新构建可使用 --replace：{app}')
        info_path = app / 'Contents/Info.plist'
        if app.is_symlink() or not info_path.is_file() or (
                plistlib.loads(info_path.read_bytes()).get('CFBundleIdentifier') != 'app.husk.nce.mac'):
            raise SystemExit('输出目录不属于本工具生成的应用：' + str(app))
        opened = subprocess.run(['lsof', '-t', str(app / 'Contents/MacOS/HuskNCE')],
                                capture_output=True, text=True, timeout=15)
        if opened.returncode not in (0, 1) or opened.stdout.strip():
            raise SystemExit('请先退出输出目录中的应用，再重新构建。')
        shutil.rmtree(app)
    contents = app / 'Contents'
    executable = contents / 'MacOS/HuskNCE'
    resources = contents / 'Resources'
    frameworks = contents / 'Frameworks'
    for directory in (executable.parent, resources, frameworks):
        directory.mkdir(parents=True)
    gfx = ROOT / 'work/gfxstream-src'
    libraries = [ROOT / 'work/gfxstream-build-macos-ios-api/prefix/lib/libgfxstream_backend.dylib',
                 ROOT / 'work/angle-macos/prefix/lib/libANGLE-shared.dylib']
    minimum = max((re.search(r'\bminos (\S+)', subprocess.check_output(
        ['xcrun', 'vtool', '-show-build', lib], text=True)).group(1)
        for lib in libraries), key=lambda value: tuple(map(int, value.split('.'))))
    for source in libraries:
        target = frameworks / source.name
        shutil.copyfile(source, target)
        target.chmod(0o755)
        commands = subprocess.check_output(['otool', '-l', target], text=True)
        for entry in re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', commands):
            run(['install_name_tool', '-delete_rpath', entry, target])
        run(['install_name_tool', '-add_rpath', '@loader_path', target])
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    flags = ['-O2', '-g', '-Wall', '-Wextra', '-Werror', '-isysroot', sdk,
             f'-mmacosx-version-min={minimum}', '-I', INPUT, '-I', EXP,
             '-DNCE_PROCESS', '-DNCE_GRAPHICS', '-DNCE_IOS_APP']
    sources = [EXP / name for name in ['lkl-main.c', 'lkl-host.c', 'lkl-process-host.c',
               'nce.c', 'entry.S', 'programs.c', 'memory.c', 'execute-check.c', 'compute.c', 'input.c', 'session.c']]
    sources += [INPUT / 'payload.S', EXP / 'guest.S', EXP / 'graphics/host-graphics.cpp',
                *[EXP / 'macos' / name for name in ('Display.mm', 'Storage.mm')]]
    sources += [EXP / "app/DeviceStore.mm", EXP / "ios/Bridge.c"]
    objects = []
    with (OUT / 'build.log').open('w') as log:
        for index, source in enumerate(sources):
            obj = OUT / f'{index}-{source.stem}.o'
            extra = ['-DNATIVE'] if source.name == 'guest.S' else []
            if source.suffix in ('.mm', '.cpp'):
                extra += ['-std=c++17', '-Wno-return-type-c-linkage']
            if source.suffix == '.mm':
                extra += ['-fobjc-arc']
            if source.name == 'host-graphics.cpp':
                for include in ('host/include', 'host/features/include', 'common/base/include'):
                    extra += ['-isystem', gfx / include]
            run(['xcrun', 'clang', *flags, *extra, '-c', source, '-o', obj], stdout=log, stderr=subprocess.STDOUT)
            objects.append(obj)
        run(['xcrun', 'swiftc', '-O', '-parse-as-library', '-import-objc-header', EXP / 'macos/Bridge.h',
             '-target', f'arm64-apple-macosx{minimum}', *sorted((EXP / 'app').glob('*.swift')),
             EXP / 'macos/Application.swift', *objects, '-Xlinker', '-lc++',
             '-framework', 'AppKit', '-framework', 'Metal', '-framework', 'QuartzCore',
             '-L', frameworks, '-lgfxstream_backend', '-Xlinker', '-rpath', '-Xlinker', '@executable_path/../Frameworks',
             '-o', executable], stdout=log, stderr=subprocess.STDOUT)
    for name in ('lkl-image.bin', 'system.img', 'android-data-seed.img'):
        run(['cp', '-c', INPUT / name, resources / name])
    (resources / 'ResourceSizes.plist').write_bytes(plistlib.dumps({
        name: (resources / name).stat().st_size for name in ('lkl-image.bin', 'system.img', 'android-data-seed.img')}))
    from package_native_linux_image import write_runtime
    write_runtime(resources)
    write_notices(resources)
    iconset = OUT / 'AppIcon.iconset'
    iconset.mkdir(exist_ok=True)
    source = ROOT / 'src/app/Husk/Assets.xcassets/AppIcon.appiconset/Icon-Default.png'
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            suffix = '@2x' if scale == 2 else ''
            run(['sips', '-z', size * scale, size * scale, source,
                 '--out', iconset / f'icon_{size}x{size}{suffix}.png'], stdout=subprocess.DEVNULL)
    run(['iconutil', '-c', 'icns', iconset, '-o', resources / 'AppIcon.icns'])
    build = 'Mac-M1-' + datetime.now().astimezone().strftime('%Y%m%d-%H%M%S')
    info = {'CFBundleIdentifier': 'app.husk.nce.mac', 'CFBundleExecutable': executable.name,
            'CFBundleName': 'Husk NCE', 'CFBundleDisplayName': 'Husk NCE', 'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': '0.13.0', 'CFBundleVersion': '2', 'NCEBuildID': build,
            'CFBundleIconFile': 'AppIcon', 'LSMinimumSystemVersion': minimum,
            'LSArchitecturePriority': ['arm64'], 'NSHighResolutionCapable': True,
            'NSSupportsAutomaticTermination': False, 'NSSupportsSuddenTermination': False}
    (contents / 'Info.plist').write_bytes(plistlib.dumps(info))
    entitlements = OUT / 'HuskNCE.entitlements'
    entitlements.write_bytes(plistlib.dumps({'com.apple.security.cs.allow-unsigned-executable-memory': True,
                                           'com.apple.security.cs.disable-library-validation': True}))
    for library in frameworks.iterdir():
        audit_libraries(library)
        run(['codesign', '--force', '--sign', '-', '--options', 'runtime', '--timestamp=none', library])
    audit_libraries(executable)
    run(['codesign', '--force', '--sign', '-', '--options', 'runtime', '--timestamp=none',
         '--entitlements', entitlements, app])
    run(['codesign', '--verify', '--deep', '--strict', app])
    for path, expected in verified.items():
        if digest(path) != expected:
            raise SystemExit('打包期间输入发生变化：' + str(path))
    manifest = {'build': build, 'minimum_macos': minimum, 'platform': platform.platform(),
                'app': str(app), 'signing': 'ad-hoc hardened runtime',
                'inputs': {str(p.relative_to(ROOT)): h for p, h in verified.items()},
                'files': {str(p.relative_to(app)): digest(p) for p in app.rglob('*') if p.is_file()}}
    (OUT / 'package.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(app)


if __name__ == '__main__':
    main()
