#!/usr/bin/env python3
"""只读扫描固定 HyperOS 3 输入中的配置与核心 ELF、APK、APEX 样本。"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import zipfile

from inspect_hyperos_input import ROOT, MANIFEST_SHA256, digest, require

DEBUGFS = Path('/opt/homebrew/opt/e2fsprogs/sbin/debugfs')
EROFS = Path('/opt/homebrew/bin/dump.erofs')
READELF = Path('/opt/homebrew/opt/llvm/bin/llvm-readelf')
BUILD_TOOLS = Path('/opt/homebrew/share/android-commandlinetools/build-tools/36.0.0')
JAVA = '/opt/homebrew/opt/openjdk@17'
LIMIT = 1024**3
CORE = (
    '/init.environ.rc', '/system/build.prop', '/system_ext/etc/build.prop', '/product/etc/build.prop',
    '/system/bin/app_process64', '/system/bin/app_process32', '/system/bin/surfaceflinger',
    '/system/bin/mediaserver64', '/system/bin/mediaserver', '/system/bin/drmserver',
    '/system/lib64/libandroid_runtime.so', '/system/lib64/libbinder.so', '/system/lib64/libgui.so',
    '/system/lib64/libEGL.so', '/system/lib64/libui.so',
    '/system/framework/framework.jar', '/system/framework/services.jar', '/system/framework/framework-res.apk',
    '/system_ext/framework/miui-framework.jar', '/system_ext/framework/xiaomi-framework.jar',
    '/system_ext/framework/miui-services.jar', '/system/priv-app/SettingsProvider/SettingsProvider.apk',
    '/system_ext/priv-app/MiuiSystemUI/MiuiSystemUI.apk', '/system_ext/priv-app/Settings/Settings.apk',
    '/product/priv-app/MiuiHome/MiuiHome.apk',
    '/system/apex/com.android.art.capex', '/system/apex/com.android.runtime.apex',
)
CONFIG_DIRS = ('init', 'permissions', 'sysconfig', 'vintf', 'selinux', 'classpaths', 'linkerconfig')


def elf_layout(data):
    require(data[:4] == b'\x7fELF' and data[5] == 1, '需要小端 ELF')
    bits = data[4] * 32
    require(bits in (32, 64), '未知 ELF class')
    offset = struct.unpack_from('<Q' if bits == 64 else '<I', data, 32 if bits == 64 else 28)[0]
    size, count = struct.unpack_from('<HH', data, 54 if bits == 64 else 42)
    require(size == (56 if bits == 64 else 32) and offset + size * count <= len(data), 'ELF 程序头越界')
    loads = []
    for i in range(count):
        entry = struct.unpack_from('<IIQQQQQQ' if bits == 64 else '<IIIIIIII', data, offset + size * i)
        if bits == 64:
            kind, flags, file_offset, address, _, filesz, memsz, align = entry
        else:
            kind, file_offset, address, _, filesz, memsz, flags, align = entry
        if kind == 1:
            require(file_offset + filesz <= len(data) and filesz <= memsz, 'ELF LOAD 越界')
            loads.append(dict(offset=file_offset, address=address, filesz=filesz, memsz=memsz,
                              flags=flags, align=align, congruent_16k=(address - file_offset) % 16384 == 0))
    conflicts = []
    for i, left in enumerate(loads):
        for j, right in enumerate(loads[i + 1:], i + 1):
            if left['flags'] != right['flags'] and left['memsz'] and right['memsz']:
                first = max(left['address'] // 16384, right['address'] // 16384)
                end = min((left['address'] + left['memsz'] + 16383) // 16384,
                          (right['address'] + right['memsz'] + 16383) // 16384)
                if first < end:
                    conflicts.append(dict(segments=[i, j], first_page=first, pages=end - first))
    return dict(bits=bits, machine=struct.unpack_from('<H', data, 18)[0], loads=loads,
                permission_overlap_16k=conflicts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system-directory', required=True, type=Path)
    parser.add_argument('--vendor-directory', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    system, vendor, out = [p.resolve() for p in (args.system_directory, args.vendor_directory, args.output)]
    require(all(p.is_relative_to(ROOT / 'work') for p in (system, vendor, out)), '路径必须位于项目 work 内')
    inputs = {}
    for directory, filename in ((system, 'system.ext4'), (vendor, 'vendor.erofs')):
        extraction = json.loads((directory / 'extraction.json').read_text())
        item = extraction['files']['images/system.img']
        require(extraction['passed'] and extraction['manifest_sha256'] == MANIFEST_SHA256, '输入尚未通过发布校验')
        path = directory / filename
        require(path.stat().st_size == item['bytes'] and digest(path) == item['sha256'], '输入分区与提取记录不符')
        inputs[filename] = item
    out.mkdir(exist_ok=False)
    (out / 'logs').mkdir()
    report = dict(scope='Selected offline files; no boot, installation, runtime dependency or performance validation',
                  script_sha256=digest(Path(__file__).resolve()), inputs=inputs, files={}, inventory={}, commands=[],
                  missing=[], skipped=[], extracted_bytes=0, passed=False)

    def run(command, label):
        result = subprocess.run(list(map(str, command)), capture_output=True, timeout=120,
                                env={**os.environ, 'JAVA_HOME': JAVA})
        index = len(report['commands'])
        log = out / 'logs' / f'{index:04d}-{label}.txt'
        log.write_bytes(result.stdout + b'\n[stderr]\n' + result.stderr)
        report['commands'].append(dict(command=list(map(str, command)), exit_code=result.returncode,
                                       log=str(log.relative_to(out))))
        return result

    def safe(path):
        require(re.fullmatch(r'/[A-Za-z0-9_./@+\-:\[\]=%,]*', path) and '..' not in PurePosixPath(path).parts,
                '无效镜像路径：' + path)

    def listing(path):
        safe(path)
        if path in report['inventory']:
            return report['inventory'][path]
        result = run([DEBUGFS, '-R', 'ls -p ' + path, system / 'system.ext4'], 'directory')
        require(result.returncode == 0, 'debugfs 目录读取失败')
        entries = []
        for line in result.stdout.decode().splitlines():
            match = re.fullmatch(r'/(\d+)/(\d+)/\d+/\d+/([^/]+)/([^/]*)/', line)
            if match and match[3] not in ('.', '..'):
                entry = dict(path=path.rstrip('/') + '/' + match[3], inode=int(match[1]),
                             mode=int(match[2], 8), bytes=int(match[4] or '0'))
                safe(entry['path'])
                entries.append(entry)
        report['inventory'][path] = entries
        return entries

    def write(data, target):
        require(report['extracted_bytes'] + len(data) <= LIMIT, '文件扫描超过 1 GiB 预算')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        report['extracted_bytes'] += len(data)

    def inspect(path, target):
        data = target.read_bytes()
        item = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), file=str(target.relative_to(out)))
        report['files'][path] = item
        if data.startswith(b'\x7fELF'):
            item['elf'] = elf_layout(data)
            result = run([READELF, '-h', '-l', '-d', '-V', target], 'elf')
            item['readelf_exit'] = result.returncode
            item['needed'] = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', result.stdout.decode())
        if target.suffix in ('.apk', '.apex', '.capex'):
            result = run([BUILD_TOOLS / 'apksigner', 'verify', '--verbose', '--print-certs', target], 'signature')
            item['signature_exit'] = result.returncode
            item['certificate_sha256'] = re.findall(r'Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]+)', result.stdout.decode())
            with zipfile.ZipFile(target) as archive:
                item['archive_members'] = archive.namelist()
                item['native'] = []
                for entry in archive.infolist():
                    if entry.filename.startswith('lib/') and entry.filename.endswith('.so'):
                        require(entry.file_size <= 128 * 1024**2, 'APK 原生库超过样本预算')
                        library = archive.read(entry)
                        with target.open('rb') as stream:
                            stream.seek(entry.header_offset + 26)
                            name_len, extra_len = struct.unpack('<HH', stream.read(4))
                        position = entry.header_offset + 30 + name_len + extra_len
                        item['native'].append(dict(path=entry.filename, sha256=hashlib.sha256(library).hexdigest(),
                                                   compressed=entry.compress_type != 0, zip_offset=position,
                                                   zip_aligned_16k=position % 16384 == 0, elf=elf_layout(library)))
                for member in ('apex_manifest.pb', 'apex_manifest.json', 'apex_pubkey'):
                    if member in archive.namelist():
                        item[member + '_sha256'] = hashlib.sha256(archive.read(member)).hexdigest()
                if target.suffix in ('.capex', '.apex'):
                    apex_dir = target.parent / (target.name + '.contents')
                    for member in ('original_apex', 'apex_manifest.pb', 'apex_manifest.json', 'apex_pubkey', 'apex_payload.img'):
                        if member not in archive.namelist():
                            continue
                        require(archive.getinfo(member).file_size <= 256 * 1024**2, 'APEX 成员超过样本预算')
                        destination = apex_dir / ('original.apex' if member == 'original_apex' else member)
                        write(archive.read(member), destination)
                        inspect(path + '!' + member, destination)
                        if member == 'apex_manifest.pb':
                            result = subprocess.run(['/opt/homebrew/bin/protoc', '--decode_raw'],
                                                    input=destination.read_bytes(), capture_output=True, timeout=30)
                            item['manifest_decode_exit'] = result.returncode
                            item['manifest_decoded'] = result.stdout.decode()
                        if member == 'apex_payload.img':
                            inspect_apex_payload(path, destination)
            if target.suffix == '.apk':
                result = run([BUILD_TOOLS / 'aapt2', 'dump', 'badging', target], 'badging')
                item['badging_exit'] = result.returncode
                item['package_line'] = next((line for line in result.stdout.decode().splitlines() if line.startswith('package:')), None)
                result = run([BUILD_TOOLS / 'aapt2', 'dump', 'xmltree', '--file', 'AndroidManifest.xml', target], 'manifest')
                item['manifest_exit'] = result.returncode
                item['manifest_log'] = report['commands'][-1]['log']

    def inspect_apex_payload(path, payload):
        with payload.open('rb') as stream:
            prefix = stream.read(2048)
        erofs = prefix[1024:1028] == bytes.fromhex('e2e1f5e0')
        require(erofs or prefix[1080:1082] == b'\x53\xef', '未知 APEX 文件系统格式')
        if 'com.android.runtime.' in path:
            names = ('/bin/linker64', '/lib64/bionic/libc.so', '/lib64/bionic/libdl.so', '/lib64/bionic/libm.so')
        elif 'com.android.art.' in path:
            names = ('/lib64/libart.so', '/lib64/libartbase.so', '/bin/dex2oat64', '/bin/artd')
        else:
            return
        for name in names:
            command = [EROFS, '--cat', '--path=' + name, payload] if erofs else [DEBUGFS, '-R', 'cat ' + name, payload]
            result = run(command, 'apex-file')
            if result.returncode or not result.stdout.startswith(b'\x7fELF'):
                report['missing'].append(path + '!apex_payload.img' + name)
                continue
            destination = payload.parent / 'files' / name.lstrip('/')
            write(result.stdout, destination)
            (out / report['commands'][-1]['log']).write_bytes(result.stderr)
            inspect(path + '!apex_payload.img' + name, destination)

    def extract(entry):
        path = entry['path']
        if path in report['files']:
            return
        if entry['mode'] & 0o170000 != 0o100000:
            report['skipped'].append(dict(path=path, mode=entry['mode']))
            return
        require(report['extracted_bytes'] + entry['bytes'] <= LIMIT, '文件扫描超过 1 GiB 预算')
        result = run([DEBUGFS, '-R', f"cat <{entry['inode']}>", system / 'system.ext4'], 'extract')
        require(result.returncode == 0 and len(result.stdout) == entry['bytes'], 'debugfs 文件大小不匹配：' + path)
        target = out / 'files' / path.lstrip('/')
        write(result.stdout, target)
        # 二进制内容已写入 files，不在命令日志中保留第二份。
        (out / report['commands'][-1]['log']).write_bytes(result.stderr)
        inspect(path, target)

    def configs(path):
        for entry in listing(path):
            if entry['mode'] & 0o170000 == 0o040000:
                configs(entry['path'])
            elif not entry['path'].endswith('.fsv_meta'):
                extract(entry)

    def vendor_file(path):
        safe(path)
        result = run([EROFS, '--cat', '--path=' + path, vendor / 'vendor.erofs'], 'vendor')
        if result.returncode:
            report['missing'].append('/vendor' + path)
            return
        target = out / 'files/vendor' / path.lstrip('/')
        write(result.stdout, target)
        inspect('/vendor' + path, target)

    def vendor_configs(path):
        result = run([EROFS, '--ls', '--path=' + path, vendor / 'vendor.erofs'], 'vendor-directory')
        if result.returncode:
            report['missing'].append('/vendor' + path)
            return
        entries = []
        for line in result.stdout.decode().splitlines():
            match = re.fullmatch(r'\s+(\d+)\s+(\d+)\s+([^ ]+)', line)
            if match and match[3] not in ('.', '..'):
                child = path.rstrip('/') + '/' + match[3]
                safe(child)
                entries.append(dict(path=child, type=int(match[2])))
                if match[2] == '2':
                    vendor_configs(child)
                elif match[2] == '1':
                    vendor_file(child)
        report['inventory']['/vendor' + path] = entries

    try:
        for path in CORE:
            found = next((entry for entry in listing(str(PurePosixPath(path).parent)) if entry['path'] == path), None)
            if found:
                extract(found)
            else:
                report['missing'].append(path)
        for partition in ('system', 'system_ext', 'product'):
            for folder in CONFIG_DIRS:
                configs('/' + partition + '/etc/' + folder)
            for folder in ('framework', 'lib', 'lib64', 'bin', 'apex'):
                listing('/' + partition + '/' + folder)
        # vendor 的这些配置直接从 system.img 中的逻辑分区读取。
        for path in ('/etc/fstab.ranchu', '/build.prop'):
            vendor_file(path)
        for path in ('/etc/init', '/etc/vintf', '/etc/selinux'):
            vendor_configs(path)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (out / 'scan.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(out / 'scan.json')
    print(json.dumps(dict(files=len(report['files']), bytes=report['extracted_bytes'], missing=report['missing'])))


if __name__ == '__main__':
    main()
