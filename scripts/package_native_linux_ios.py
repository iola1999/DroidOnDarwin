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


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, timeout=600, **kwargs)


def plist(path, value):
    path.write_bytes(plistlib.dumps(value, sort_keys=False))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--rebuild-kernel', action='store_true')
    args = parser.parse_args()
    if args.rebuild_kernel or not (INPUT / 'lkl-summary.json').exists():
        run(['python3', ROOT / 'scripts/probe_lkl.py', '--processes'])
    # A package must contain the kernel and fixtures that passed the host run.
    summary = json.loads((INPUT / 'lkl-summary.json').read_text())
    for name, digest in summary['input_sha256'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise SystemExit(f'{name} changed; run with --rebuild-kernel')
    if hashlib.sha256((INPUT / 'lkl-image.bin').read_bytes()).hexdigest() != summary['kernel_sha256']:
        raise SystemExit('Kernel image does not match the validated manifest')
    OUT.mkdir(parents=True, exist_ok=True)
    build_id = 'N2-iOS-' + time.strftime('%Y%m%d-%H%M%S')
    info = plistlib.loads((ROOT / 'src/app/Husk/Info.plist').read_bytes())
    for key in ['CFBundleIcons', 'CFBundleIcons~ipad', 'HuskPerformanceBuild']:
        info.pop(key, None)
    info.update(CFBundleDisplayName='Husk NCE', NCEBuildID=build_id,
                UILaunchScreen={},
                UISupportedInterfaceOrientations=['UIInterfaceOrientationPortrait',
                    'UIInterfaceOrientationPortraitUpsideDown',
                    'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
                CFBundleShortVersionString='0.8.1', CFBundleVersion='36')
    plist(OUT / 'Info.plist', info)
    # Deployment entitlements are supplied by iLoader. This ad-hoc signature is
    # for archive integrity; get-task-allow is needed by the in-app JIT helper.
    plist(OUT / 'Husk.entitlements', {'get-task-allow': True})
    sources = [{'path': str(EXP / name)} for name in
               ['lkl-main.c', 'lkl-host.c', 'lkl-process-host.c', 'memory.c', 'compute.c', 'nce.c', 'entry.S',
                'ios/Bridge.c', 'ios/NativeLinuxApp.swift']]
    sources += [{'path': str(EXP / 'guest.S'), 'compilerFlags': ['-DNATIVE']}]
    sources += [{'path': str(ROOT / name)} for name in
                ['src/ios-jit/husk-ios-jit.c', 'src/ios-jit/husk-brk.S',
                 'src/app/Husk/JITBuiltIn.swift', 'src/app/Husk/JITPairingFileStore.swift',
                 'src/app/HuskJITHelper/HuskJITMessages.swift', 'src/app/Husk/Assets.xcassets']]
    sources += [{'path': str(path), 'buildPhase': 'resources'} for path in
                [INPUT / 'lkl-image.bin', ROOT / 'src/app/Husk/Resources/husk-jit.js']]
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
                    'HEADER_SEARCH_PATHS': [str(INPUT), str(EXP), str(ROOT / 'src/ios-jit')],
                    'GCC_PREPROCESSOR_DEFINITIONS': ['$(inherited)', 'NCE_PROCESS=1', 'NCE_IOS_APP=1',
                                                     'HUSK_PREPARED_JIT_ONLY=1'],
                    'GCC_OPTIMIZATION_LEVEL': '2',
                    'LD_RUNPATH_SEARCH_PATHS': ['$(inherited)', '@executable_path/Frameworks']}},
                'dependencies': [{'target': 'HuskJITHelper'}]},
            'HuskJITHelper': {
                'type': 'app-extension', 'platform': 'iOS',
                'sources': [{'path': str(ROOT / 'src/app/HuskJITHelper')}],
                'settings': {'base': {
                    'PRODUCT_BUNDLE_IDENTIFIER': 'com.husk.app.JITHelper',
                    'INFOPLIST_FILE': str(ROOT / 'src/app/HuskJITHelper/Info.plist'),
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
    for component in [embedded, app / 'PlugIns/HuskJITHelper.appex', app]:
        entitlements = ['--entitlements', OUT / 'Husk.entitlements'] if component == app else []
        run(['codesign', '--force', '--sign', '-', '--timestamp=none', *entitlements, component])
        run(['codesign', '--verify', '--strict', component])
    run(['codesign', '--verify', '--deep', '--strict', app])
    package = OUT / 'Husk-NativeLinux-N2.ipa'
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
                'device_execution': 'pending', 'mac_validation': summary['results'],
                'source_sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                  for path in [Path(__file__).resolve(), *sorted(EXP.glob('ios/*')),
                                               *[Path(item['path']) for item in sources]] if path.is_file()}}

    (OUT / 'package.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
