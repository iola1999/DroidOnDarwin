#!/usr/bin/env python3
"""生成同证书的 Java/JNI 两版本 APK，核对签名、包内容和 16 KiB 原生库布局。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'experiments/native-linux/apk-fixture'
SDK = Path('/opt/homebrew/share/android-commandlinetools')
BUILD_TOOLS = SDK / 'build-tools/36.0.0'
NDK = Path('/opt/homebrew/share/android-ndk')
JAVA = Path('/opt/homebrew/opt/openjdk@17/bin')
ANDROID_JAR = SDK / 'platforms/android-35/android.jar'
PASSWORD = 'droidondarwin-fixture'  # 只保护本地测试签名材料，不用于发布。
PAGE = 16384


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    result = subprocess.run(list(map(str, command)), check=True, capture_output=True, text=True,
                            timeout=120, env={**os.environ, 'JAVA_HOME': str(JAVA.parent)})
    return result.stdout + result.stderr


def elf_loads(data):
    if data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('JNI 库需要为小端 ARM64 ELF')
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if size != 56:
        raise ValueError('ELF 程序头大小不匹配')
    loads = []
    for index in range(count):
        kind, flags, file_offset, address, _, filesz, memsz, alignment = struct.unpack_from('<IIQQQQQQ', data, offset + index * size)
        if kind == 1:
            if alignment < PAGE or alignment % PAGE or (address - file_offset) % PAGE:
                raise ValueError('JNI LOAD 段不满足 16 KiB 映射条件')
            loads.append(dict(offset=file_offset, vaddr=address, filesz=filesz, memsz=memsz, alignment=alignment, flags=flags))
    if not loads:
        raise ValueError('JNI 库没有 LOAD 段')
    return loads


def append_file(archive, name, data, compressed):
    entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    entry.compress_type = zipfile.ZIP_DEFLATED if compressed else zipfile.ZIP_STORED
    entry.external_attr = 0o100644 << 16
    archive.writestr(entry, data)


def build_one(stage, keystore, mode, version):
    folder = stage / f'{mode}-v{version}'
    folder.mkdir()
    package = 'app.droidondarwin.fixture.' + mode
    manifest = folder / 'AndroidManifest.xml'
    manifest.write_text(f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{package}" android:versionCode="{version}" android:versionName="{version}">
    <uses-sdk android:minSdkVersion="35" android:targetSdkVersion="35" />
    <application android:label="Droid fixture {mode} v{version}" android:allowBackup="false"
        android:debuggable="true" android:extractNativeLibs="false">
        <activity android:name="app.droidondarwin.fixture.MainActivity" android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
''')
    config = folder / 'BuildConfig.java'
    config.write_text('package app.droidondarwin.fixture;\nfinal class BuildConfig {\n'
                      f'    static final int VERSION = {version};\n'
                      f'    static final boolean USE_JNI = {str(mode == "jni").lower()};\n}}\n')
    classes, dex = folder / 'classes', folder / 'dex'
    classes.mkdir()
    dex.mkdir()
    run([JAVA / 'javac', '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-bootclasspath', ANDROID_JAR,
         '-d', classes, SOURCE / 'MainActivity.java', SOURCE / 'NativeBridge.java', config])
    inputs = sorted(p for p in classes.rglob('*.class') if mode == 'jni' or p.name != 'NativeBridge.class')
    run([JAVA / 'java', '-cp', BUILD_TOOLS / 'lib/d8.jar', 'com.android.tools.r8.D8', '--min-api', '35',
         '--lib', ANDROID_JAR, '--output', dex, *inputs])
    library = None
    if mode == 'jni':
        library = folder / 'libfixture.so'
        compiler = NDK / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang'
        run([compiler, '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror', f'-DFIXTURE_VALUE={100 + version}',
             '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-Wl,--build-id=none',
             '-Wl,-soname,libfixture.so', SOURCE / 'native-value.c', '-o', library])
    raw, aligned = folder / 'raw.apk', folder / 'aligned.apk'
    run([BUILD_TOOLS / 'aapt2', 'link', '-I', ANDROID_JAR, '--manifest', manifest, '-o', raw])
    with zipfile.ZipFile(raw, 'a') as archive:
        append_file(archive, 'classes.dex', (dex / 'classes.dex').read_bytes(), True)
        if library:
            append_file(archive, 'lib/arm64-v8a/libfixture.so', library.read_bytes(), False)
    run([BUILD_TOOLS / 'zipalign', '-P', '16', '-f', '4', raw, aligned])
    apk = stage / f'{mode}-v{version}.apk'
    run([BUILD_TOOLS / 'apksigner', 'sign', '--ks', keystore, '--ks-key-alias', 'fixture',
         '--ks-pass', 'pass:' + PASSWORD, '--key-pass', 'pass:' + PASSWORD,
         '--v1-signing-enabled', 'true', '--v2-signing-enabled', 'true', '--v3-signing-enabled', 'true',
         '--v4-signing-enabled', 'false', '--out', apk, aligned])
    signature = run([BUILD_TOOLS / 'apksigner', 'verify', '--verbose', '--print-certs', apk])
    certificate = re.search(r'Signer #1 certificate SHA-256 digest: ([0-9a-fA-F]{64})', signature)
    if not certificate:
        raise ValueError('签名检查未返回证书 SHA-256')
    run([BUILD_TOOLS / 'zipalign', '-c', '-P', '16', '-v', '4', apk])
    badging = run([BUILD_TOOLS / 'aapt2', 'dump', 'badging', apk])
    if f"package: name='{package}' versionCode='{version}'" not in badging:
        raise ValueError('APK 包名或版本与请求不一致')
    native = None
    with zipfile.ZipFile(apk) as archive:
        if archive.read('classes.dex') != (dex / 'classes.dex').read_bytes():
            raise ValueError('签名 APK 的 DEX 内容不匹配')
        libraries = [name for name in archive.namelist() if name.startswith('lib/')]
        if libraries != (['lib/arm64-v8a/libfixture.so'] if library else []):
            raise ValueError('APK 原生库列表不匹配')
        if library:
            entry = archive.getinfo(libraries[0])
            data = archive.read(entry)
            with apk.open('rb') as stream:
                stream.seek(entry.header_offset + 26)
                name_size, extra_size = struct.unpack('<HH', stream.read(4))
            position = entry.header_offset + 30 + name_size + extra_size
            if entry.compress_type != zipfile.ZIP_STORED or position % PAGE or data != library.read_bytes():
                raise ValueError('APK 原生库的存储、对齐或内容不匹配')
            native = dict(path=entry.filename, sha256=digest(library), zip_offset=position,
                          zip_stored=True, page_bytes=PAGE, elf_loads=elf_loads(data))
    return dict(apk=apk.name, sha256=digest(apk), bytes=apk.stat().st_size, package=package, version=version,
                expected_value=100 + version, certificate_sha256=certificate[1].lower(),
                native=native, signature_verification=signature.strip(),
                package_badging=badging.strip(), installed=False, executed=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'work/native-linux-nce/apk-fixture')
    args = parser.parse_args()
    output = args.output.resolve()
    if output == ROOT / 'work' or not output.is_relative_to(ROOT / 'work'):
        parser.error('产物目录必须位于本项目 work/ 下')
    properties = (NDK / 'source.properties').read_text()
    if 'Pkg.Revision = 29.0.14206865' not in properties:
        parser.error('需要 Android NDK 29.0.14206865')
    output.mkdir(parents=True, exist_ok=True)
    keystore = output / 'fixture.p12'
    if not keystore.exists():
        run([JAVA / 'keytool', '-genkeypair', '-noprompt', '-storetype', 'PKCS12', '-keystore', keystore,
             '-alias', 'fixture', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
             '-dname', 'CN=DroidOnDarwin APK Fixture', '-storepass', PASSWORD, '-keypass', PASSWORD])
        keystore.chmod(0o600)
    sources = [Path(__file__).resolve(), *sorted(SOURCE.glob('*'))]
    report = dict(scope='Offline APK build and signature/layout checks; Android installation and execution pending',
                  source_sha256={str(p.relative_to(ROOT)): digest(p) for p in sources if p.is_file()},
                  tools=dict(sdk=str(SDK), build_tools=str(BUILD_TOOLS),
                             build_tools_properties=(BUILD_TOOLS / 'source.properties').read_text().strip(),
                             aapt2=run([BUILD_TOOLS / 'aapt2', 'version']).strip(),
                             apksigner=run([BUILD_TOOLS / 'apksigner', 'version']).strip(),
                             java=run([JAVA / 'java', '-version']).strip(),
                             ndk=str(NDK), ndk_properties=properties.strip(), android_jar_sha256=digest(ANDROID_JAR)),
                  keystore=keystore.name, artifacts=[])
    with tempfile.TemporaryDirectory(prefix='.apk-build-', dir=output) as temporary:
        stage = Path(temporary)
        for mode in ('java', 'jni'):
            for version in (1, 2):
                report['artifacts'].append(build_one(stage, keystore, mode, version))
        if len({item['certificate_sha256'] for item in report['artifacts']}) != 1:
            raise ValueError('四份 APK 的签名证书不一致')
        for item in report['artifacts']:
            (stage / item['apk']).replace(output / item['apk'])
        summary = stage / 'manifest.json'
        summary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        summary.replace(output / 'manifest.json')
    print(output / 'manifest.json')
    for item in report['artifacts']:
        print(item['apk'], item['sha256'], item['certificate_sha256'])


if __name__ == '__main__':
    main()
