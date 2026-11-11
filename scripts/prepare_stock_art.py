#!/usr/bin/env python3
"""编译标准 JNI fixture，核对并准备原始 SDK36 ART 组件；不运行客体。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

from build_apk_fixture import elf_loads

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'experiments/native-linux/stock-art'
SDK = Path('/opt/homebrew/share/android-commandlinetools')
JAVA = Path('/opt/homebrew/opt/openjdk@17/bin')
NDK = Path('/opt/homebrew/share/android-ndk')
TOOL = NDK / 'toolchains/llvm/prebuilt/darwin-x86_64'
RUNTIME_MANIFEST_SHA256 = '4b1a45b0ee51725c68f1c29765958d0b60b45a5f2325bedfc61764cd49232249'
DEPENDENCIES_SHA256 = 'c9462a396acb6d39d8052d01220a534ae8483ab6d3977f82413d401ff7e40e5f'
BOOT_JARS = (
    'com.android.art/javalib/core-oj.jar',
    'com.android.art/javalib/core-libart.jar',
    'com.android.art/javalib/okhttp.jar',
    'com.android.art/javalib/bouncycastle.jar',
    'com.android.art/javalib/apache-xml.jar',
    'com.android.i18n/javalib/core-icu4j.jar',
    'com.android.conscrypt/javalib/conscrypt.jar',
)
PUBLIC_LIBRARIES = 'etc/public.libraries.txt'
DATA_FILES = ('com.android.runtime/bin/linker64', PUBLIC_LIBRARIES, *BOOT_JARS,
              'com.android.i18n/etc/icu/icudt76l.dat',
              'com.android.tzdata/etc/tz/tzdata', 'com.android.tzdata/etc/tz/tz_version')
SEARCH_PATHS = ('/apex/com.android.art/lib64', '/apex/com.android.runtime/lib64/bionic',
                '/apex/com.android.i18n/lib64', '/apex/com.android.os.statsd/lib64', '/system/lib64')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, commands):
    command = list(map(str, command))
    commands.append(command)
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=120,
                            env={**os.environ, 'JAVA_HOME': str(JAVA.parent)})
    return result.stdout + result.stderr


def guest_path(relative):
    if relative.startswith('com.android.'):
        return '/apex/' + relative
    if relative.startswith(('lib64/', 'etc/')):
        return '/system/' + relative
    raise ValueError('未定义 guest 路径：' + relative)


def inputs(runtime, manifest_path, commands):
    lock_path = SOURCE / 'dependencies.json'
    if sha(lock_path) != DEPENDENCIES_SHA256:
        raise ValueError('固定 ART 依赖报告摘要不匹配')
    if sha(manifest_path) != RUNTIME_MANIFEST_SHA256:
        raise ValueError('原始 SDK36 runtime manifest 摘要不匹配')
    lock = json.loads(lock_path.read_text())
    manifest = json.loads(manifest_path.read_text())
    if lock['manifest_sha256'] != RUNTIME_MANIFEST_SHA256 or lock['missing']:
        raise ValueError('ART 依赖报告与 runtime manifest 不一致')
    records = {}
    prefix = 'work/native-linux-nce/android/android36/'
    for name, library in lock['libraries'].items():
        if not library['path'].startswith(prefix):
            raise ValueError('依赖路径与 SDK36 目录不一致：' + name)
        relative = library['path'][len(prefix):]
        if Path(relative).name != name or manifest['files'][relative] != library['sha256']:
            raise ValueError('依赖身份与 runtime manifest 不一致：' + name)
        records[relative] = dict(library)
    for relative in DATA_FILES:
        records[relative] = {'sha256': manifest['files'][relative]}
    public = runtime / PUBLIC_LIBRARIES
    if sha(public) != manifest['files'][PUBLIC_LIBRARIES]:
        raise ValueError('原始公共库清单摘要不匹配')
    preload = []
    for line in public.read_text().splitlines():
        fields = line.split('#', 1)[0].split()
        if fields and not {'nopreload', '32'} & set(fields[1:]):
            preload.append(fields[0])
    # NativeLoader preloads this original list during JNI_CreateJavaVM.
    # Resolve its DT_NEEDED closure using the component's actual search order.
    libraries = {Path(relative).name: relative for relative in records if relative.endswith('.so')}
    search = [path.removeprefix('/apex/').removeprefix('/system/') for path in SEARCH_PATHS]
    pending, visited = list(preload), set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        if name not in libraries:
            candidates = [path + '/' + name for path in search if path + '/' + name in manifest['files']]
            if not candidates:
                raise ValueError('公共库预加载依赖缺失：' + name)
            relative = candidates[0]
            libraries[name] = relative
            records[relative] = {'sha256': manifest['files'][relative]}
        relative = libraries[name]
        if 'needed' not in records[relative]:
            path = runtime / relative
            if sha(path) != records[relative]['sha256']:
                raise ValueError('公共库预加载依赖摘要不匹配：' + relative)
            dynamic = run([TOOL / 'bin/llvm-readelf', '--dynamic', path], commands)
            records[relative]['needed'] = re.findall(r'Shared library: \[([^]]+)\]', dynamic)
        pending.extend(records[relative]['needed'])
    for relative, record in records.items():
        path = runtime / relative
        if sha(path) != record['sha256']:
            raise ValueError('原始文件摘要不匹配：' + relative)
        size = path.stat().st_size
        if 'bytes' in record and record['bytes'] != size:
            raise ValueError('原始文件长度不匹配：' + relative)
        record.update(source=str(path), bytes=size, guest=guest_path(relative))
        if relative.endswith('.so') or relative == 'com.android.runtime/bin/linker64':
            record['loads'] = elf_loads(path.read_bytes())
            dynamic = run([TOOL / 'bin/llvm-readelf', '--dynamic', path], commands)
            needed = re.findall(r'Shared library: \[([^]]+)\]', dynamic)
            if 'needed' in record and needed != record['needed']:
                raise ValueError('DT_NEEDED 与固定报告不一致：' + relative)
            record['needed'] = needed
        elif relative.endswith('.jar'):
            with zipfile.ZipFile(path) as archive:
                if 'classes.dex' not in archive.namelist() or archive.testzip() is not None:
                    raise ValueError('boot JAR 无完整 DEX：' + relative)
    if len({record['guest'] for record in records.values()}) != len(records):
        raise ValueError('多个输入映射到同一 guest 路径')
    return manifest, records


def linker_config():
    lines = ['dir.stock_art = /system/bin', '', '[stock_art]',
             'additional.namespaces = com_android_art,com_android_i18n,com_android_conscrypt,com_android_os_statsd',
             'namespace.default.isolated = false', 'namespace.default.visible = true',
             'namespace.default.search.paths = ' + ':'.join(SEARCH_PATHS)]
    for name in ('com_android_art', 'com_android_i18n', 'com_android_conscrypt', 'com_android_os_statsd'):
        lines += [f'namespace.{name}.isolated = false', f'namespace.{name}.visible = true',
                  f'namespace.{name}.links = default', f'namespace.{name}.link.default.allow_all_shared_libs = true']
    return '\n'.join(lines) + '\n'


def verify(output):
    report = json.loads((output / 'manifest.json').read_text())
    if (report['input_manifest_sha256'] != RUNTIME_MANIFEST_SHA256 or
            report['dependencies_sha256'] != DEPENDENCIES_SHA256 or report['runtime_executed'] is not False):
        raise ValueError('组件 manifest 身份或阶段不匹配')
    original_manifest = Path(report['source_manifest'])
    if sha(original_manifest) != RUNTIME_MANIFEST_SHA256:
        raise ValueError('原始 SDK36 runtime manifest 摘要不匹配')
    originals = json.loads(original_manifest.read_text())['files']
    for name, expected in report['source_sha256'].items():
        if sha(ROOT / name) != expected:
            raise ValueError('fixture 构建源码摘要不匹配：' + name)
    for guest, record in report['files'].items():
        path = output / 'root' / guest.lstrip('/')
        if not path.resolve().is_relative_to((output / 'root').resolve()):
            raise ValueError('组件文件越过 staging root：' + guest)
        if not path.is_file() or path.stat().st_size != record['bytes'] or sha(path) != record['sha256']:
            raise ValueError('组件文件缺失或摘要不匹配：' + guest)
        if path.stat().st_mode & 0o777 != record['mode']:
            raise ValueError('组件文件权限不匹配：' + guest)
        if record['kind'] == 'original' and originals[record['source_relative']] != record['sha256']:
            raise ValueError('组件文件与原始 manifest 摘要不匹配：' + guest)
    expected = {'/' + str(p.relative_to(output / 'root')) for p in (output / 'root').rglob('*') if p.is_file()}
    if expected != set(report['files']):
        raise ValueError('组件文件集合与 manifest 不一致')
    return report


def prepare(runtime, manifest_path, output):
    if not output.resolve().is_relative_to((ROOT / 'work').resolve()):
        raise ValueError('输出必须位于本工作树 work 目录')
    commands = []
    runtime, manifest_path = runtime.resolve(), manifest_path.resolve()
    manifest, records = inputs(runtime, manifest_path, commands)
    output.mkdir(parents=True, exist_ok=False)
    root = output / 'root'
    directories = ('/system/bin', '/system/lib64', '/linkerconfig', '/data/stock-art',
                   '/data/dalvik-cache/arm64', '/tmp')
    for name in directories:
        (root / name.lstrip('/')).mkdir(parents=True, exist_ok=True)
    files = {}
    for relative, record in records.items():
        target = root / record['guest'].lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(record['source'], target)
        target.chmod(0o755 if relative.endswith('linker64') else 0o644)
        if sha(target) != record['sha256']:
            raise ValueError('复制后摘要不匹配：' + relative)
        files[record['guest']] = {'kind': 'original', 'source_relative': relative,
                                  'sha256': record['sha256'], 'bytes': record['bytes']}
    configuration = root / 'linkerconfig/ld.config.txt'
    configuration.write_text(linker_config())
    classes, dex = output / 'classes', output / 'dex'
    classes.mkdir()
    dex.mkdir()
    android_jar = SDK / 'platforms/android-35/android.jar'
    javac = [JAVA / 'javac', '-encoding', 'UTF-8', '-source', '8', '-target', '8',
             '-bootclasspath', android_jar, '-d', classes, SOURCE / 'StockArtProbe.java']
    run(javac, commands)
    d8 = [JAVA / 'java', '-cp', SDK / 'build-tools/36.0.0/lib/d8.jar', 'com.android.tools.r8.D8',
          '--min-api', '35', '--lib', android_jar, '--output', dex, *sorted(classes.rglob('*.class'))]
    run(d8, commands)
    executable = root / 'system/bin/stock-art-probe'
    compile = [TOOL / 'bin/aarch64-linux-android35-clang', '-std=gnu11', '-O2', '-g',
               '-Wall', '-Wextra', '-Werror', '-fPIE', '-pie', '-pthread', '-Wl,-z,max-page-size=16384',
               '-Wl,-z,common-page-size=16384', '-Wl,--build-id=none',
               SOURCE / 'probe.c', '-ldl', '-llog', '-o', executable]
    run(compile, commands)
    loads = elf_loads(executable.read_bytes())
    dynamic = run([TOOL / 'bin/llvm-readelf', '--dynamic', executable], commands)
    needed = re.findall(r'Shared library: \[([^]]+)\]', dynamic)
    lock = json.loads((SOURCE / 'dependencies.json').read_text())
    if not set(needed) <= lock['libraries'].keys():
        raise ValueError('fixture 引入未固定依赖：' + repr(needed))
    headers = run([TOOL / 'bin/llvm-readelf', '--program-headers', executable], commands)
    if '[Requesting program interpreter: /system/bin/linker64]' not in headers:
        raise ValueError('fixture ELF interpreter 与标准 Android 路径不一致')
    symbols = run([TOOL / 'bin/llvm-readelf', '--dyn-syms', executable], commands)
    if 'nce_art_' in symbols or 'nce_art_' in (SOURCE / 'probe.c').read_text():
        raise ValueError('标准 JNI fixture 引用了私有 ART 入口')
    (output / 'fixture-dynamic.txt').write_text(dynamic)
    (output / 'fixture-program-headers.txt').write_text(headers)
    (output / 'fixture-symbols.txt').write_text(symbols)
    destination = root / 'data/stock-art/StockArtProbe.dex'
    shutil.copyfile(dex / 'classes.dex', destination)
    if not destination.read_bytes().startswith(b'dex\n'):
        raise ValueError('D8 产物缺少 DEX header')
    for path in [configuration, executable, destination]:
        guest = '/' + str(path.relative_to(root))
        files[guest] = {'kind': 'fixture', 'sha256': sha(path), 'bytes': path.stat().st_size}
    # PT_INTERP uses the ordinary Android alias, with identical linker bytes.
    interpreter = root / 'system/bin/linker64'
    original_linker = records['com.android.runtime/bin/linker64']
    shutil.copyfile(original_linker['source'], interpreter)
    interpreter.chmod(0o755)
    files['/system/bin/linker64'] = {'kind': 'original', 'source_relative': 'com.android.runtime/bin/linker64',
                                   'sha256': original_linker['sha256'], 'bytes': original_linker['bytes']}
    for guest, record in files.items():
        record['mode'] = (root / guest.lstrip('/')).stat().st_mode & 0o777
    report = {
        'scope': 'Prepared original SDK36 ART inputs and compiled standard JNI fixture; guest runtime not executed',
        'runtime_executed': False, 'source_runtime': str(runtime), 'source_manifest': str(manifest_path),
        'input_manifest_sha256': sha(manifest_path), 'dependencies_sha256': sha(SOURCE / 'dependencies.json'),
        'identity': {key: manifest[key] for key in ('url', 'archive_sha1')},
        'source_sha256': {str(p.relative_to(ROOT)): sha(p) for p in
                          [SOURCE / 'probe.c', SOURCE / 'StockArtProbe.java', SOURCE / 'dependencies.json',
                           Path(__file__), ROOT / 'scripts/build_apk_fixture.py']},
        'tools': {'ndk': (NDK / 'source.properties').read_text(), 'fixture_min_api': 35,
                  'javac': run([JAVA / 'javac', '-version'], commands).strip(),
                  'clang': run([TOOL / 'bin/clang', '--version'], commands).strip(),
                  'd8_sha256': sha(SDK / 'build-tools/36.0.0/lib/d8.jar'), 'android_jar_sha256': sha(android_jar),
                  'jni_header_sha256': sha(TOOL / 'sysroot/usr/include/jni.h')},
        'commands': commands, 'inputs': records, 'files': files,
        'directories': sorted('/' + str(p.relative_to(root)) for p in root.rglob('*') if p.is_dir()),
        'fixture_elf': {'loads': loads, 'needed': needed, 'interpreter': '/system/bin/linker64'},
        'guest_command': ['/system/bin/stock-art-probe', ':'.join(guest_path(p) for p in BOOT_JARS)],
        'required_guest_services': ['Linux procfs mounted at /proc', 'readable /dev/null and /dev/urandom',
                                    'writable staging /data and /tmp', 'sparse guest address range beyond 8.25 GiB'],
        'limitations': ['Relaxed component linker namespaces; product namespace policy untested',
                        'Static dependencies only; further dynamic loads and property requirements untested',
                        'JIT requested; compilation and execution of generated Java code unverified'],
    }
    if sha(manifest_path) != RUNTIME_MANIFEST_SHA256 or any(
            sha(Path(record['source'])) != record['sha256'] for record in records.values()):
        raise ValueError('准备期间原始组件输入发生变化')
    (output / 'manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    verify(output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=ROOT / 'work/native-linux-nce/android/android36')
    parser.add_argument('--manifest', type=Path, default=ROOT / 'work/native-linux-nce/android/android36-runtime.json')
    output = parser.add_mutually_exclusive_group(required=True)
    output.add_argument('--output', type=Path)
    output.add_argument('--verify', type=Path)
    args = parser.parse_args()
    report = verify(args.verify.resolve()) if args.verify else prepare(args.runtime, args.manifest, args.output.resolve())
    print(json.dumps({'status': 'prepared', 'runtime_executed': False, 'files': len(report['files']),
                      'original_bytes': sum(r['bytes'] for r in report['files'].values() if r['kind'] == 'original')}, ensure_ascii=False))


if __name__ == '__main__':
    main()
