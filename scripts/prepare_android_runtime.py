#!/usr/bin/env python3
"""从固定版本的 Android 16 SDK 镜像提取用户态运行时，校验后清理中间镜像。"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1] / 'work/native-linux-nce/android'
DEST = ROOT / 'android36'
URL = 'https://dl.google.com/android/repository/sys-img/google_apis/arm64-v8a-ps16k-36_r07.zip'
SHA1 = 'dda632745023567113d9954101709c6526a3c250'
SIZE = 1875087037
APEX = ('com.android.devicelock.apex', 'com.android.i18n.apex', 'com.android.runtime.apex',
        'com.android.virt.apex', 'com.google.android.adbd.capex', 'com.google.android.adservices.capex',
        'com.google.android.appsearch.capex', 'com.google.android.art.capex', 'com.google.android.bt.apex',
        'com.google.android.cellbroadcast.capex', 'com.google.android.configinfrastructure.capex',
        'com.google.android.conscrypt.capex', 'com.google.android.crashrecovery.capex',
        'com.google.android.extservices.capex', 'com.google.android.resolv.capex',
        'com.google.android.healthfitness.apex', 'com.google.android.ipsec.capex',
        'com.google.android.media.capex', 'com.google.android.mediaprovider.capex',
        'com.google.android.nfcservices.capex', 'com.google.android.ondevicepersonalization.capex',
        'com.google.android.os.statsd.apex', 'com.google.android.permission.capex',
        'com.google.android.profiling.capex', 'com.google.android.scheduling.capex',
        'com.google.android.sdkext.apex', 'com.google.android.tethering.capex',
        'com.google.android.tzdata6.apex', 'com.google.android.uwb.capex', 'com.google.android.wifi.capex')
DEBUGFS = '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'
SYSTEM_DIRECTORIES = ('lib64', 'etc', 'bin', 'framework', 'fonts', 'usr')


def digest(path, algorithm='sha256'):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def partition_extents(image):
    # This pinned SDK image contains GPT, then a single-device linear LP layout.
    image_bytes = image.stat().st_size
    with image.open('rb') as source:
        source.seek(512)
        header = source.read(92)
        if len(header) != 92 or header[:8] != b'EFI PART':
            raise RuntimeError('Expected GPT system image')
        offset, count, size = struct.unpack_from('<QII', header, 72)
        if size < 128 or size % 8 or not count or offset * 512 + count * size > image_bytes:
            raise RuntimeError('Invalid GPT entry range')
        source.seek(offset * 512)
        entries = [source.read(size) for _ in range(count)]
        matches = [e for e in entries if e[56:128].decode('utf-16-le').rstrip('\0') == 'super']
        if len(matches) != 1:
            raise RuntimeError('Expected one GPT super partition')
        super_entry = matches[0]
        base = struct.unpack_from('<Q', super_entry, 32)[0] * 512
        end = (struct.unpack_from('<Q', super_entry, 40)[0] + 1) * 512
        if not 0 <= base < base + 12288 + 128 <= end <= image_bytes:
            raise RuntimeError('Invalid GPT super range')
        source.seek(base + 12288)
        header = bytearray(source.read(128))
        expected = bytes(header[12:44])
        header[12:44] = bytes(32)
        if header[:4] != b'0PLA' or hashlib.sha256(header).digest() != expected:
            raise RuntimeError('Invalid LP header')
        table_bytes = struct.unpack_from('<I', header, 44)[0]
        if not table_bytes or base + 12288 + 128 + table_bytes > end:
            raise RuntimeError('Invalid LP table range')
        tables = source.read(table_bytes)
        if hashlib.sha256(tables).digest() != header[48:80]:
            raise RuntimeError('Invalid LP tables')
        po, pn, ps = struct.unpack_from('<III', header, 80)
        eo, en, es = struct.unpack_from('<III', header, 92)
        if ps < 52 or es < 24 or po + pn * ps > len(tables) or eo + en * es > len(tables):
            raise RuntimeError('Invalid LP table descriptors')
        partitions = [struct.unpack_from('<36sIIII', tables, po + i * ps) for i in range(pn)]
        result = {}
        for partition in partitions:
            name = partition[0].rstrip(b'\0').decode('ascii')
            if not name or name in result or partition[2] + partition[3] > en:
                raise RuntimeError('Invalid LP partition extent index')
            extents = []
            for i in range(partition[2], partition[2] + partition[3]):
                sectors, kind, start, device = struct.unpack_from('<QIQI', tables, eo + i * es)
                if kind or device:
                    raise RuntimeError('Expected a linear extent on device zero')
                absolute, extent_bytes = base + start * 512, sectors * 512
                if not extent_bytes or not base <= absolute < absolute + extent_bytes <= end:
                    raise RuntimeError('LP extent exceeds the original super partition')
                extents.append({'offset': absolute, 'bytes': extent_bytes, 'start_sector': start, 'sectors': sectors})
            result[name] = extents
    return {'image_bytes': image_bytes, 'super_offset': base, 'lp_header_sha256': expected.hex(),
            'lp_tables_sha256': bytes(header[48:80]).hex(), 'partitions': result}


def system_partition(image, output, name="system"):
    extents = partition_extents(image)['partitions'][name]
    with image.open('rb') as source, output.open('wb') as target:
        for extent in extents:
            source.seek(extent['offset'])
            remaining = extent['bytes']
            while remaining:
                chunk = source.read(min(remaining, 4 * 1024 * 1024))
                if not chunk:
                    raise RuntimeError('Truncated system extent')
                target.write(chunk)
                remaining -= len(chunk)


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    manifest = ROOT / 'android36-runtime.json'
    if manifest.exists():
        prior = json.loads(manifest.read_text())
        if prior['archive_sha1'] == SHA1 and prior.get('apex') == list(APEX) and prior.get('system_directories') == list(SYSTEM_DIRECTORIES) and all((DEST / name).is_file() and digest(DEST / name) == sha
                                                for name, sha in prior['files'].items()):
            print('Android runtime files verified')
            return
    archive = ROOT / 'android36-16k.zip'
    if not archive.exists():
        print('Downloading Android 16 16 KiB image', flush=True)
        with urllib.request.urlopen(URL, timeout=60) as source, archive.open('wb') as target:
            shutil.copyfileobj(source, target, 4 * 1024 * 1024)
    if archive.stat().st_size != SIZE or digest(archive, 'sha1') != SHA1:
        raise RuntimeError('SDK archive checksum mismatch')
    image, partition = ROOT / 'system36.img', ROOT / 'system36-part.img'
    DEST.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as package:
        (DEST / 'NOTICE.txt').write_bytes(package.read('arm64-v8a/NOTICE.txt'))
        if not image.exists():
            with package.open('arm64-v8a/system.img') as source, image.open('wb') as target:
                shutil.copyfileobj(source, target, 4 * 1024 * 1024)
    system_partition(image, partition)
    intermediates = [archive, image, partition]
    with (ROOT / 'extract.log').open('w') as log:
        def debug(command, filesystem):
            subprocess.run([DEBUGFS, '-R', command, filesystem], check=True,
                           stdout=log, stderr=log, timeout=120)

        for name in APEX:
            path = ROOT / name
            intermediates.append(path)
            debug(f'dump /system/apex/{name} {path}', partition)
            if name.endswith('.capex'):
                with zipfile.ZipFile(path) as package:
                    content = package.read('original_apex')
                path = ROOT / name.replace('.capex', '.apex')
                intermediates.append(path)
                path.write_bytes(content)
            payload = ROOT / 'payload.img'
            intermediates.append(payload)
            with zipfile.ZipFile(path) as package:
                payload.write_bytes(package.read('apex_payload.img'))
            module = name.removesuffix('.apex').removesuffix('.capex').replace('com.google.android.', 'com.android.')
            if module == 'com.android.tzdata6': module = 'com.android.tzdata'
            destination = DEST / module
            destination.mkdir(exist_ok=True)
            debug(f'rdump / {destination}', payload)
        for name in SYSTEM_DIRECTORIES:
            debug(f'rdump /system/{name} {DEST}', partition)
    for name in ('com.android.runtime/bin/linker64', 'com.android.runtime/lib64/bionic/libc.so',
                 'com.android.art/bin/dalvikvm64', 'com.android.art/lib64/libart.so'):
        if (DEST / name).read_bytes()[:4] != b'\x7fELF':
            raise RuntimeError(f'Missing extracted ELF: {name}')
    files = {str(path.relative_to(DEST)): digest(path) for path in sorted(DEST.rglob('*'))
             if not path.is_symlink() and path.is_file()}
    manifest.write_text(json.dumps(dict(url=URL, archive_sha1=SHA1, apex=APEX, system_directories=SYSTEM_DIRECTORIES, files=files), indent=2) + '\n')
    for path in intermediates:
        path.unlink(missing_ok=True)
    print(f'Extracted and hashed {len(files)} runtime files; intermediate images removed')


if __name__ == '__main__':
    main()
