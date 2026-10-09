#!/usr/bin/env python3
"""构建可用 iLoader 重签安装的原生 Linux + NCE 测试包。"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
INPUT = ROOT / 'work/native-linux-nce/processes'
OUT = ROOT / 'work/native-linux-nce/ios'
REVISION = 'N4-G1'


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=600, **kwargs)


def plist(path, value):
    path.write_bytes(plistlib.dumps(value, sort_keys=False))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rebuild-kernel', action='store_true')
    args = parser.parse_args()
    if args.rebuild_kernel or not (INPUT / 'lkl-summary.json').exists():
        run(['python3', ROOT / 'scripts/probe_lkl.py', '--processes', '--graphics-angle', ROOT / 'work/angle-macos/prefix/lib', '--graphics-window'])
    # A package must contain the kernel and fixtures that passed the host run.
    summary = json.loads((INPUT / 'lkl-summary.json').read_text())
    for name, digest in summary['input_sha256'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise SystemExit(f'{name} changed; run with --rebuild-kernel')
    if hashlib.sha256((INPUT / 'lkl-image.bin').read_bytes()).hexdigest() != summary['kernel_sha256']:
        raise SystemExit('Kernel image does not match the validated manifest')
    for name, digest in summary['generated_sha256'].items():
        if hashlib.sha256((INPUT / name).read_bytes()).hexdigest() != digest:
            raise SystemExit(f'{name} does not match the validated manifest')
    if not any(r.get('test') == 'linux_native_gles_shader_frames' and r.get('passed')
               for r in summary['results']):
        raise SystemExit('Run the graphics-enabled Linux probe before packaging')
    if not summary.get('window_results') or summary['window_results'][-1].get('result') != 'passed':
        raise SystemExit('Run the visible Metal window regression before packaging')
    for name, digest in summary['graphics_libraries_sha256'].items():
        if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
            raise SystemExit('Host graphics changed; rerun the graphics probe: ' + name)
    angle_build = json.loads((ROOT / 'build/ios-arm64/sysroot/angle-manifest.json').read_text())
    if hashlib.sha256(Path(angle_build['library']).read_bytes()).hexdigest() != angle_build['sha256']:
        raise SystemExit('ANGLE library does not match its build manifest')
    if hashlib.sha256((ROOT / 'scripts/build_angle_ios.sh').read_bytes()).hexdigest() != angle_build['script_sha256']:
        raise SystemExit('Rebuild ANGLE; build script changed')
    gfx = ROOT / 'work/gfxstream-src'
    graphics = [ROOT / 'build/ios-arm64/sysroot/lib' / name
                for name in ['libgfxstream_backend.dylib', 'libANGLE-shared.dylib']]
    gfx_build = json.loads((ROOT / 'work/gfxstream-build-ios/manifest.json').read_text())
    for name, digest in gfx_build['inputs'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise SystemExit('Rebuild iOS graphics; input changed: ' + name)
    for name, digest in gfx_build['libraries'].items():
        if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
            raise SystemExit('Rebuild iOS graphics; library changed: ' + name)
    OUT.mkdir(parents=True, exist_ok=True)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', '-isysroot', sdk,
         EXP / 'ios/Bridge.c', EXP / 'ios/detach-test.c', '-o', OUT / 'detach-test'])
    with (OUT / 'detach-test.log').open('w') as log:
        run([OUT / 'detach-test'], stdout=log)
    run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', '-isysroot', sdk, '-I', EXP, '-I', INPUT,
         EXP / 'execute-check.c', EXP / 'memory.c', EXP / 'nce.c', EXP / 'entry.S',
         EXP / 'programs.c', INPUT / 'payload.S',
         EXP / 'ios/execute-test.c', '-o', OUT / 'execute-test'])
    with (OUT / 'execute-test.log').open('w') as log:
        run([OUT / 'execute-test'], stdout=log)
    run(['node', ROOT / 'tests/ios-jit/script-test.cjs'])
    run(['xcrun', 'swiftc', '-O', EXP / 'ios/ProbeLogReader.swift',
         ROOT / 'tests/ios-jit/LogReaderTests.swift', '-o', OUT / 'log-reader-test'])
    with (OUT / 'log-reader-test.log').open('w') as log:
        run([OUT / 'log-reader-test'], stdout=log)
    build_id = REVISION + '-iOS-' + time.strftime('%Y%m%d-%H%M%S')
    info = plistlib.loads((ROOT / 'src/app/Husk/Info.plist').read_bytes())
    for key in ['CFBundleIcons', 'CFBundleIcons~ipad', 'HuskPerformanceBuild']:
        info.pop(key, None)
    info.update(CFBundleDisplayName='Husk NCE', NCEBuildID=build_id,
                UILaunchScreen={},
                UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait',
                    'UIInterfaceOrientationPortraitUpsideDown',
                    'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
                CFBundleShortVersionString='0.11.1', CFBundleVersion='78')
    plist(OUT / 'Info.plist', info)
    helper_info = plistlib.loads((ROOT / 'src/app/HuskJITHelper/Info.plist').read_bytes())
    for key in ['CFBundleShortVersionString', 'CFBundleVersion']:
        helper_info[key] = info[key]
    plist(OUT / 'Helper-Info.plist', helper_info)
    # Deployment entitlements are supplied by iLoader. This ad-hoc signature is
    # for archive integrity; get-task-allow is needed by the in-app JIT helper.
    plist(OUT / 'Husk.entitlements', {'get-task-allow': True})
    sources = [{'path': str(EXP / name)} for name in
               ['lkl-main.c', 'lkl-host.c', 'lkl-process-host.c', 'programs.c', 'memory.c', 'execute-check.c', 'compute.c', 'input.c', 'nce.c', 'entry.S',
                'ios/Bridge.c', 'ios/NativeLinuxApp.swift', 'ios/ProbeLogReader.swift', 'graphics/host-graphics.cpp']]
    sources += [{'path': str(EXP / 'guest.S'), 'compilerFlags': ['-DNATIVE']}]
    sources += [{'path': str(INPUT / 'payload.S')}]
    sources += [{'path': str(ROOT / name)} for name in
                ['src/ios-jit/husk-ios-jit.c', 'src/ios-jit/husk-brk.S',
                 'src/app/Husk/JITBuiltIn.swift', 'src/app/Husk/JITPairingFileStore.swift',
                 'src/app/HuskJITHelper/HuskJITMessages.swift', 'src/app/Husk/Assets.xcassets']]
    sources += [{'path': str(path), 'buildPhase': 'resources'} for path in
                [INPUT / 'lkl-image.bin', INPUT / 'android-data-seed.img', INPUT / 'system.img', ROOT / 'src/app/Husk/Resources/husk-jit.js',
                 ROOT / 'work/native-linux-nce/musl-1.2.5/COPYRIGHT',
                 ROOT / 'work/native-linux-nce/dash-0.5.12/COPYING',
                 ROOT / 'work/native-linux-nce/compiler-rt-20.1.8.src/LICENSE.TXT']]
    android = ROOT / 'work/native-linux-nce/android'
    notices = OUT / 'Android-NOTICES.txt'
    notice_paths = ['android36/NOTICE.txt', 'art/NOTICE', 'bionic-source/libc/NOTICE',
                    'bionic-source/linker/NOTICE', 'bionic-source/libdl/NOTICE', 'scudo/LICENSE.TXT',
                    'libunwind-20.1.8.src/LICENSE.TXT', 'fmt/LICENSE', 'dlmalloc/NOTICE',
                    'libnativehelper/NOTICE', 'cpu_features/LICENSE', 'tinyxml2/LICENSE.txt',
                    'lz4/lib/LICENSE', 'lzma/NOTICE', 'libbase/NOTICE', 'liblog/liblog/NOTICE',
                    'vixl/LICENCE', 'boringssl/src/LICENSE']
    notices.write_bytes(b''.join(f'\n===== {name} =====\n'.encode() + (android / name).read_bytes()
                                for name in notice_paths))
    sources += [{'path': str(notices), 'buildPhase': 'resources'},
                {'path': str(android / 'android36/etc/NOTICE.xml.gz'), 'buildPhase': 'resources'}]
    graphics_notices = OUT / 'Graphics-NOTICES.txt'
    angle_source = ROOT / 'third_party/gpu/webkit/Source/ThirdParty/ANGLE'
    license_paths = [gfx / 'LICENSE', *sorted((gfx / 'third_party').rglob('LICENSE*')),
                     *sorted(p for p in angle_source.rglob('*') if p.is_file() and
                             p.name.upper().startswith(('LICENSE', 'COPYING')) and '.xcarchive/' not in str(p))]
    graphics_notices.write_bytes(b''.join(('\n===== ' + str(p.relative_to(ROOT)) + ' =====\n').encode()
                                        + p.read_bytes() for p in license_paths if p.is_file()))
    sources.append({'path': str(graphics_notices), 'buildPhase': 'resources'})
    framework = ROOT / 'src/app/Frameworks/StikJIT.xcframework'
    spec = {
        'name': 'HuskNCE',
        'options': {'deploymentTarget': {'iOS': '26.0'}},
        'settings': {'base': {'ARCHS': 'arm64', 'ONLY_ACTIVE_ARCH': 'YES', 'SWIFT_VERSION': '5.0',
                              'CODE_SIGNING_ALLOWED': 'NO', 'SWIFT_OPTIMIZATION_LEVEL': '-O',
                              'ENABLE_BITCODE': 'NO', 'TARGETED_DEVICE_FAMILY': '1'}},
        'targets': {
            'Husk': {
                'type': 'application', 'platform': 'iOS', 'sources': sources,
                'settings': {'base': {
                    'PRODUCT_BUNDLE_IDENTIFIER': 'com.husk.app',
                    'INFOPLIST_FILE': str(OUT / 'Info.plist'),
                    'SWIFT_OBJC_BRIDGING_HEADER': str(EXP / 'ios/Bridge.h'),
                    'CODE_SIGN_ENTITLEMENTS': str(OUT / 'Husk.entitlements'),
                    'ASSETCATALOG_COMPILER_APPICON_NAME': 'AppIcon',
                    'HEADER_SEARCH_PATHS': [str(INPUT), str(EXP), str(ROOT / 'src/ios-jit'), str(gfx / 'host/include'), str(gfx / 'host/features/include'), str(gfx / 'common/base/include')],
                    'CLANG_CXX_LANGUAGE_STANDARD': 'c++17',
                    'LIBRARY_SEARCH_PATHS': ['$(inherited)', str(graphics[0].parent)],
                    'OTHER_CPLUSPLUSFLAGS': ['$(inherited)', '-Wno-return-type-c-linkage'],
                    'GCC_PREPROCESSOR_DEFINITIONS': ['$(inherited)', 'NCE_PROCESS=1', 'NCE_IOS_APP=1',
                                                     'HUSK_PREPARED_JIT_ONLY=1', 'NCE_GRAPHICS=1'],
                    'GCC_OPTIMIZATION_LEVEL': '2',
                    'LD_RUNPATH_SEARCH_PATHS': ['$(inherited)', '@executable_path/Frameworks']}},
                'dependencies': [{'target': 'HuskJITHelper'}, *[{'framework': str(p), 'embed': True, 'link': p.name == 'libgfxstream_backend.dylib'} for p in graphics]]},
            'HuskJITHelper': {
                'type': 'app-extension', 'platform': 'iOS',
                'sources': [{'path': str(ROOT / 'src/app/HuskJITHelper'), 'excludes': ['Info.plist']}],
                'settings': {'base': {
                    'PRODUCT_BUNDLE_IDENTIFIER': 'com.husk.app.JITHelper',
                    'INFOPLIST_FILE': str(OUT / 'Helper-Info.plist'),
                    'APPLICATION_EXTENSION_API_ONLY': 'YES',
                    'LD_RUNPATH_SEARCH_PATHS': ['$(inherited)', '@executable_path/../../Frameworks']}},
                'dependencies': [{'framework': str(framework), 'embed': False}]}}}
    (OUT / 'project.json').write_text(json.dumps(spec, indent=2) + '\n')
    run(['xcodegen', 'generate', '--spec', OUT / 'project.json', '--project', OUT])
    with (OUT / 'build.log').open('w') as log:
        run(['xcodebuild', '-project', OUT / 'HuskNCE.xcodeproj', '-scheme', 'Husk',
             '-configuration', 'Release', '-sdk', 'iphoneos', '-derivedDataPath', OUT / 'DerivedData',
             'CODE_SIGNING_ALLOWED=NO', 'build'], stdout=log, stderr=subprocess.STDOUT)
    app = OUT / 'DerivedData/Build/Products/Release-iphoneos/Husk.app'
    embedded = app / 'Frameworks/StikJIT.framework'
    if embedded.exists():
        shutil.rmtree(embedded)
    shutil.copytree(framework / 'ios-arm64/StikJIT.framework', embedded)
    for directory in ['Headers', 'Modules']:
        shutil.rmtree(embedded / directory, ignore_errors=True)
    plist(embedded / 'Info.plist', {
        'CFBundleIdentifier': 'com.husk.app.StikJIT', 'CFBundleExecutable': 'StikJIT',
        'CFBundleName': 'StikJIT', 'CFBundlePackageType': 'FMWK', 'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleShortVersionString': '1.9.0', 'CFBundleVersion': '1.9.0',
        'MinimumOSVersion': '17.4', 'CFBundleSupportedPlatforms': ['iPhoneOS']})
    for component in [*[app / 'Frameworks' / p.name for p in graphics], embedded, app / 'PlugIns/HuskJITHelper.appex', app]:
        entitlements = ['--entitlements', OUT / 'Husk.entitlements'] if component == app else []
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', *entitlements, component])
        run(['codesign', '--verify', '--strict', component])
    run(['codesign', '--verify', '--deep', '--strict', app])
    package = OUT / f'Husk-NativeLinux-{REVISION}.ipa'
    with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(app.rglob('*')):
            if path.is_file():
                archive.write(path, Path('Payload/Husk.app') / path.relative_to(app))
    with zipfile.ZipFile(package) as archive:
        if archive.testzip() is not None:
            raise SystemExit('IPA archive validation failed')
    manifest = {'build': build_id, 'ipa': str(package), 'bytes': package.stat().st_size,
                'sha256': hashlib.sha256(package.read_bytes()).hexdigest(),
                'kernel_sha256': summary['kernel_sha256'],
                'generated_sha256': summary['generated_sha256'],
                'device_execution': 'pending',
                'graphics': {'gfxstream_revision': gfx_build['revision'],
                             'angle_build': angle_build,
                             'libraries_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in graphics}}, 'mac_validation': summary['results'],
                'dispatch_validation': summary['dispatch_results'],
                'window_validation': summary.get('window_results'),
                'source_sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                  for path in [Path(__file__).resolve(), ROOT / 'src/ios-jit/husk-ios-jit.h', *sorted(EXP.glob('ios/*')),
                                               *[Path(item['path']) for item in sources]] if path.is_file()}}

    (OUT / 'package.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({key: manifest[key] for key in ['build', 'ipa', 'bytes', 'sha256', 'device_execution']}, indent=2))


if __name__ == '__main__':
    main()
