#!/usr/bin/env python3
"""构建可用 iLoader 重签安装的原生 Linux + NCE 测试包。"""
import argparse
import hashlib
import json
import os
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
REVISION = '0.18.0'


def run(args, timeout=600, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=timeout, **kwargs)


def plist(path, value):
    path.write_bytes(plistlib.dumps(value, sort_keys=False))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rebuild-kernel', action='store_true')
    args = parser.parse_args()
    if args.rebuild_kernel or not (INPUT / 'lkl-summary.json').exists():
        run(['python3', ROOT / 'scripts/probe_lkl.py', '--processes', '--graphics-angle', ROOT / 'work/angle-macos/prefix/lib'],
            env={**os.environ, 'NCE_MM_MODE': 'demand', 'NCE_SYSTEM_SERVER_PROBE': '1'}, timeout=1800)
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
    for mode in ('results', 'dispatch_results'):
        records = summary.get(mode, [])
        if not any(r.get('phase') == 'linux_mapping_policy' and r.get('mode') == 'demand' for r in records):
            raise SystemExit('Desktop package requires demand mapping validation: ' + mode)
        passed = {r.get('test') for r in records if r.get('passed')}
        required = {'android_vold_user_storage', 'android_system_boot_completed',
                    'android_application_data_persistence', 'android_keystore_persistence',
                    'android_desktop_launch_commands', 'android_system_process_groups_stopped',
                    'linux_data_readonly_shutdown', 'android_clock_controls_ready',
                    'android_clock_alarm_activity', 'android_preview_clock_data_preserved'}
        if missing := required - passed:
            raise SystemExit(f'Desktop validation missing in {mode}: {sorted(missing)}')
        visible = {r.get('package') for r in records
                   if r.get('test') == 'android_desktop_visible_activity' and r.get('passed')}
        if not {'com.android.launcher3', 'com.android.deskclock',
                'com.google.android.documentsui'} <= visible:
            raise SystemExit('Desktop application pixels were not checked: ' + mode)
    for test in ('android_application_data_persistence', 'android_keystore_persistence'):
        if not any(r.get('test') == test and r.get('passed') and r.get('restored')
                   for r in summary['dispatch_results']):
            raise SystemExit('Restart persistence was not checked: ' + test)
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
    run(['xcrun', 'swiftc', '-O', EXP / 'app/ProbeLogReader.swift',
         ROOT / 'tests/ios-jit/LogReaderTests.swift', '-o', OUT / 'log-reader-test'])
    with (OUT / 'log-reader-test.log').open('w') as log:
        run([OUT / 'log-reader-test'], stdout=log)
    from package_native_linux_image import write_runtime
    write_runtime(OUT)
    plist(OUT / 'ResourceSizes.plist', {name: (INPUT / name).stat().st_size for name in ('lkl-image.bin', 'system.img', 'android-data-seed.img')})
    build_id = REVISION + '-iOS-' + time.strftime('%Y%m%d-%H%M%S')
    info = plistlib.loads((ROOT / 'src/app/Info.plist').read_bytes())
    for key in ['CFBundleIcons', 'CFBundleIcons~ipad', 'DroidOnDarwinPerformanceBuild']:
        info.pop(key, None)
    info.update(CFBundleDisplayName='DroidOnDarwin', DroidOnDarwinBuildID=build_id,
                UILaunchScreen={},
                UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait',
                    'UIInterfaceOrientationPortraitUpsideDown',
                    'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
                CFBundleShortVersionString='0.18.0', CFBundleVersion='87')
    info['UTExportedTypeDeclarations'] = [{'UTTypeIdentifier': 'app.droidondarwin.image', 'UTTypeConformsTo': ['com.apple.package'], 'UTTypeDescription': 'DroidOnDarwin Image', 'UTTypeTagSpecification': {'public.filename-extension': ['droidimage']}}]
    info['CFBundleURLTypes'] = [{'CFBundleURLName': 'app.droidondarwin.benchmark', 'CFBundleURLSchemes': ['droidondarwin']}]
    plist(OUT / 'Info.plist', info)
    helper_info = plistlib.loads((ROOT / 'src/app/JITHelper/Info.plist').read_bytes())
    for key in ['CFBundleShortVersionString', 'CFBundleVersion']:
        helper_info[key] = info[key]
    plist(OUT / 'Helper-Info.plist', helper_info)
    # Deployment entitlements are supplied by iLoader. This ad-hoc signature is
    # for archive integrity; get-task-allow is needed by the in-app JIT helper.
    plist(OUT / 'DroidOnDarwin.entitlements', {'get-task-allow': True})
    sources = [{'path': str(EXP / name)} for name in
               ['lkl-main.c', 'lkl-host.c', 'lkl-process-host.c', 'programs.c', 'memory.c', 'execute-check.c', 'compute.c', 'input.c', 'session.c', 'nce.c', 'entry.S',
                'ios/Bridge.c', 'ios/NativeLinuxApp.swift', 'ios/Display.swift', 'ios/JITSettingsView.swift', 'app/DeviceStore.mm', 'macos/Storage.mm', 'graphics/host-graphics.cpp']]
    sources += [{'path': str(path)} for path in sorted((EXP / 'app').glob('*.swift'))]
    sources += [{'path': str(EXP / 'guest.S'), 'compilerFlags': ['-DNATIVE']}]
    sources += [{'path': str(INPUT / 'payload.S')}]
    sources += [{'path': str(ROOT / name)} for name in
                ['src/ios-jit/ios-jit.c', 'src/ios-jit/jit-trap.S',
                 'src/app/JIT/JITBuiltIn.swift', 'src/app/JIT/JITPairingFileStore.swift',
                 'src/app/JITHelper/JITMessages.swift', 'src/app/Resources/Assets.xcassets']]
    sources += [{'path': str(path), 'buildPhase': 'resources'} for path in
                [OUT / 'Runtime.json', OUT / 'ResourceSizes.plist', INPUT / 'lkl-image.bin', INPUT / 'android-data-seed.img', INPUT / 'system.img', ROOT / 'src/app/Resources/jit.js',
                 ROOT / 'work/native-linux-nce/musl-1.2.5/COPYRIGHT',
                 ROOT / 'work/native-linux-nce/dash-0.5.12/COPYING',
                 ROOT / 'work/native-linux-nce/compiler-rt-20.1.8.src/LICENSE.TXT']]
    shutil.copyfile(ROOT / 'ACKNOWLEDGEMENTS.md', OUT / 'Acknowledgements.md')
    sources.append({'path': str(OUT / 'Acknowledgements.md'), 'buildPhase': 'resources'})
    framework = ROOT / 'src/app/Frameworks/StikJIT.xcframework'
    spec = {
        'name': 'DroidOnDarwin',
        'options': {'deploymentTarget': {'iOS': '26.0'}},
        'settings': {'base': {'ARCHS': 'arm64', 'ONLY_ACTIVE_ARCH': 'YES', 'SWIFT_VERSION': '5.0',
                              'CODE_SIGNING_ALLOWED': 'NO', 'SWIFT_OPTIMIZATION_LEVEL': '-O',
                              'ENABLE_BITCODE': 'NO', 'TARGETED_DEVICE_FAMILY': '1'}},
        'targets': {
            'DroidOnDarwin': {
                'type': 'application', 'platform': 'iOS', 'sources': sources,
                'settings': {'base': {
                    'PRODUCT_BUNDLE_IDENTIFIER': 'app.droidondarwin.ios',
                    'INFOPLIST_FILE': str(OUT / 'Info.plist'),
                    'SWIFT_OBJC_BRIDGING_HEADER': str(EXP / 'ios/Bridge.h'),
                    'CODE_SIGN_ENTITLEMENTS': str(OUT / 'DroidOnDarwin.entitlements'),
                    'ASSETCATALOG_COMPILER_APPICON_NAME': 'AppIcon',
                    'HEADER_SEARCH_PATHS': [str(INPUT), str(EXP), str(ROOT / 'src/ios-jit'), str(gfx / 'host/include'), str(gfx / 'host/features/include'), str(gfx / 'common/base/include')],
                    'CLANG_CXX_LANGUAGE_STANDARD': 'c++17',
                    'LIBRARY_SEARCH_PATHS': ['$(inherited)', str(graphics[0].parent)],
                    'OTHER_CPLUSPLUSFLAGS': ['$(inherited)', '-Wno-return-type-c-linkage'],
                    'GCC_PREPROCESSOR_DEFINITIONS': ['$(inherited)', 'NCE_PROCESS=1', 'NCE_IOS_APP=1',
                                                     'NCE_GRAPHICS=1'],
                    'GCC_OPTIMIZATION_LEVEL': '2',
                    'LD_RUNPATH_SEARCH_PATHS': ['$(inherited)', '@executable_path/Frameworks']}},
                'dependencies': [{'target': 'JITHelper'}, *[{'framework': str(p), 'embed': True, 'link': p.name == 'libgfxstream_backend.dylib'} for p in graphics]]},
            'JITHelper': {
                'type': 'app-extension', 'platform': 'iOS',
                'sources': [{'path': str(ROOT / 'src/app/JITHelper'), 'excludes': ['Info.plist']}],
                'settings': {'base': {
                    'PRODUCT_BUNDLE_IDENTIFIER': 'app.droidondarwin.ios.JITHelper',
                    'INFOPLIST_FILE': str(OUT / 'Helper-Info.plist'),
                    'APPLICATION_EXTENSION_API_ONLY': 'YES',
                    'LD_RUNPATH_SEARCH_PATHS': ['$(inherited)', '@executable_path/../../Frameworks']}},
                'dependencies': [{'framework': str(framework), 'embed': False}]}}}
    (OUT / 'project.json').write_text(json.dumps(spec, indent=2) + '\n')
    run(['xcodegen', 'generate', '--spec', OUT / 'project.json', '--project', OUT])
    with (OUT / 'build.log').open('w') as log:
        run(['xcodebuild', '-project', OUT / 'DroidOnDarwin.xcodeproj', '-scheme', 'DroidOnDarwin',
             '-configuration', 'Release', '-sdk', 'iphoneos', '-derivedDataPath', OUT / 'DerivedData',
             'CODE_SIGNING_ALLOWED=NO', 'clean', 'build'], stdout=log, stderr=subprocess.STDOUT)
    app = OUT / 'DerivedData/Build/Products/Release-iphoneos/DroidOnDarwin.app'
    embedded = app / 'Frameworks/StikJIT.framework'
    if embedded.exists():
        shutil.rmtree(embedded)
    shutil.copytree(framework / 'ios-arm64/StikJIT.framework', embedded)
    for directory in ['Headers', 'Modules']:
        shutil.rmtree(embedded / directory, ignore_errors=True)
    plist(embedded / 'Info.plist', {
        'CFBundleIdentifier': 'app.droidondarwin.ios.StikJIT', 'CFBundleExecutable': 'StikJIT',
        'CFBundleName': 'StikJIT', 'CFBundlePackageType': 'FMWK', 'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleShortVersionString': '1.9.0', 'CFBundleVersion': '1.9.0',
        'MinimumOSVersion': '17.4', 'CFBundleSupportedPlatforms': ['iPhoneOS']})
    for component in [*[app / 'Frameworks' / p.name for p in graphics], embedded, app / 'PlugIns/JITHelper.appex', app]:
        entitlements = ['--entitlements', OUT / 'DroidOnDarwin.entitlements'] if component == app else []
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', *entitlements, component])
        run(['codesign', '--verify', '--strict', component])
    run(['codesign', '--verify', '--deep', '--strict', app])
    package = OUT / f'DroidOnDarwin-{REVISION}.ipa'
    with zipfile.ZipFile(package, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(app.rglob('*')):
            if path.is_file():
                archive.write(path, Path('Payload/DroidOnDarwin.app') / path.relative_to(app))
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
                                  for path in [Path(__file__).resolve(), ROOT / 'scripts/package_native_linux_image.py', ROOT / 'ACKNOWLEDGEMENTS.md', ROOT / 'src/ios-jit/ios-jit.h',
                                               *sorted((ROOT / 'src/app/Resources').rglob('*')),  *sorted(EXP.glob('ios/*')),
                                               *[Path(item['path']) for item in sources]] if path.is_file()}}

    (OUT / 'package.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({key: manifest[key] for key in ['build', 'ipa', 'bytes', 'sha256', 'device_execution']}, indent=2))


if __name__ == '__main__':
    main()
