#!/usr/bin/env python3
"""检查 Mac 宿主的数据管理、坐标转换、代码签名与包内依赖。"""
import argparse
import json
from pathlib import Path
import plistlib
import subprocess
from package_native_linux_macos import ROOT, EXP, INPUT, OUT, audit_libraries, digest, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=OUT / 'DroidOnDarwin.app')
    args = parser.parse_args()
    app = args.app.resolve()
    contents = app / 'Contents'
    OUT.mkdir(parents=True, exist_ok=True)
    checks = {}
    for name, frameworks in [('Storage', ['Foundation']), ('Display', ['AppKit', 'QuartzCore', 'Metal'])]:
        binary = OUT / (name.lower() + '-tests')
        flags = [item for framework in frameworks for item in ('-framework', framework)]
        run(['xcrun', 'clang++', '-std=c++17', '-O2', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
             *flags, EXP / f'macos/{name}.mm', EXP / f'macos/{name}Tests.mm', '-o', binary])
        result = run([binary], capture_output=True, text=True, timeout=30)
        checks[name] = result.stdout.strip()
    smoke = OUT / 'Host Checks.app/Contents'
    (smoke / 'MacOS').mkdir(parents=True, exist_ok=True)
    (smoke / 'Resources').mkdir(exist_ok=True)
    (smoke / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleIdentifier': 'app.droidondarwin.host-checks', 'CFBundleExecutable': 'HostChecks',
        'CFBundleName': 'Host Checks', 'CFBundlePackageType': 'APPL'}))
    sizes = {name: 4096 for name in ('lkl-image.bin', 'system.img', 'android-data-seed.img')}
    for name in sizes:
        fixture = bytearray(4096)
        fixture[1024 + 0x38:1024 + 0x3a] = b'\x53\xef'
        (smoke / 'Resources' / name).write_bytes(fixture)
    (smoke / 'Resources/ResourceSizes.plist').write_bytes(plistlib.dumps(sizes))
    objects = []
    for source in [EXP / 'macos/Display.mm', EXP / 'macos/Storage.mm', EXP / 'macos/AppSmokeTests.mm', EXP / 'app/DeviceStore.mm', EXP / 'ios/Bridge.c']:
        obj = OUT / ('smoke-' + source.stem + '.o')
        flags = ['-std=c++17', '-fobjc-arc'] if source.suffix == '.mm' else []
        run(['xcrun', 'clang', '-O2', *flags, '-c', source, '-o', obj])
        objects.append(obj)
    run(['xcrun', 'swiftc', '-O', '-parse-as-library', '-import-objc-header', EXP / 'macos/Bridge.h',
         *sorted((EXP / 'app').glob('*.swift')), EXP / 'macos/Application.swift', EXP / 'macos/AndroidWindow.swift', *objects,
         '-Xlinker', '-lc++', '-framework', 'AppKit', '-framework', 'QuartzCore', '-framework', 'Metal',
         '-o', smoke / 'MacOS/HostChecks'])
    smoke_data = OUT / 'host-check-data'
    run([smoke / 'MacOS/HostChecks', '--data-directory', smoke_data, '--run-device'], timeout=45)
    log = max((smoke_data / 'Logs').glob('*'), key=lambda p: p.stat().st_mtime)
    line = log.read_text().splitlines()[-1]
    assert line.startswith('app lifecycle checks passed:')
    checks['app_lifecycle'] = line
    store_test = OUT / 'device-store-tests'
    run(['xcrun', 'clang++', '-std=c++17', '-O2', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-framework', 'Foundation', EXP / 'app/DeviceStore.mm', EXP / 'app/DeviceStoreTests.mm', EXP / 'macos/Storage.mm', '-o', store_test])
    checks['devices'] = run([store_test], capture_output=True, text=True, timeout=30).stdout.strip()
    session_test = OUT / 'session-model-tests'
    run(['xcrun', 'swiftc', '-O', '-parse-as-library', '-import-objc-header', EXP / 'macos/Bridge.h',
         EXP / 'app/SessionModel.swift', EXP / 'app/ProbeLogReader.swift', EXP / 'app/RunConfiguration.swift',
         ROOT / 'tests/ios-jit/SessionModelTests.swift', '-o', session_test])
    checks['session'] = run([session_test], capture_output=True, text=True, timeout=15).stdout.strip()
    benchmark_test = OUT / 'benchmark-tests'
    run(['xcrun', 'swiftc', '-O', '-parse-as-library', '-import-objc-header', EXP / 'macos/Bridge.h',
         *[EXP / 'app' / name for name in ('RunConfiguration.swift', 'BenchmarkRequest.swift', 'BenchmarkSession.swift', 'SessionModel.swift', 'ProbeLogReader.swift')],
         ROOT / 'tests/ios-jit/BenchmarkTests.swift', '-o', benchmark_test])
    checks['benchmark'] = run([benchmark_test], capture_output=True, text=True, timeout=15).stdout.strip()
    execute = OUT / 'signed-execute-test'
    run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', '-I', EXP, '-I', INPUT,
         *[EXP / name for name in ['execute-check.c', 'memory.c', 'nce.c', 'entry.S', 'programs.c']],
         INPUT / 'payload.S', EXP / 'ios/execute-test.c', '-o', execute])
    entitlement = OUT / 'execution-test.plist'
    entitlement.write_bytes(plistlib.dumps({'com.apple.security.cs.allow-unsigned-executable-memory': True}))
    run(['codesign', '--force', '--sign', '-', '--options', 'runtime', '--entitlements', entitlement, execute])
    result = run([execute], capture_output=True, text=True, timeout=30)
    (OUT / 'signed-execute-test.log').write_text(result.stdout + result.stderr)
    checks['signed_execution'] = result.stdout.splitlines()[-1]
    library_source = OUT / 'library-check.c'
    library_source.write_text('#include <dlfcn.h>\n#include <stdio.h>\n'
                             'int main(int argc, char **argv) {\n'
                             ' if (argc != 2) return 2;\n'
                             ' void *p = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);\n'
                             ' if (!p) { puts(dlerror()); return 1; }\n'
                             ' dlclose(p); return 0;\n}\n')
    library_test = OUT / 'library-check'
    run(['xcrun', 'clang', '-O2', library_source, '-o', library_test])
    library_results = {}
    for allowed in (False, True):
        entitlement.write_bytes(plistlib.dumps({'com.apple.security.cs.disable-library-validation': True} if allowed else {}))
        run(['codesign', '--force', '--sign', '-', '--options', 'runtime', '--entitlements', entitlement, library_test])
        result = subprocess.run([str(library_test), str(contents / 'Frameworks/libgfxstream_backend.dylib')],
                                capture_output=True, text=True, timeout=30)
        library_results[str(allowed)] = {'exit': result.returncode, 'message': result.stdout.strip()}
    if library_results['True']['exit'] != 0:
        raise SystemExit('带目标签名的图形库加载失败')
    checks['ad_hoc_library_validation'] = library_results
    run(['codesign', '--verify', '--deep', '--strict', app])
    for binary in [contents / 'MacOS/DroidOnDarwin', *sorted((contents / 'Frameworks').iterdir())]:
        audit_libraries(binary)
    sizes = plistlib.loads((contents / 'Resources/ResourceSizes.plist').read_bytes())
    assert all((contents / 'Resources' / name).stat().st_size == size for name, size in sizes.items())
    package = json.loads((OUT / 'package.json').read_text())
    assert all(digest(app / name) == expected for name, expected in package['files'].items())
    assert all(digest(ROOT / name) == expected for name, expected in package['inputs'].items())
    checks['bundle'] = 'signature, package hashes, input hashes, resource sizes and relocatable library paths passed'
    (OUT / 'host-checks.json').write_text(json.dumps(checks, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(checks, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
