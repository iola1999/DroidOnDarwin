#!/usr/bin/env python3
"""从运行时同版 SDK 提取桌面 APK、显示驱动及其分区配置。"""
import json
from pathlib import Path
import shutil
import subprocess
import urllib.request
import zipfile

from prepare_android_runtime import ROOT, URL, SHA1, SIZE, DEBUGFS, digest, system_partition

OUT = ROOT / 'android36-desktop'
DIRECTORIES = {
    'system': ('app', 'priv-app'),
    'system_ext': ('app', 'priv-app', 'lib64', 'framework', 'etc', 'bin'),
    'product': ('app', 'priv-app', 'lib64', 'framework', 'etc', 'overlay'),
    'vendor': ('bin', 'lib64', 'etc', 'apex'),
}


def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = OUT / 'manifest.json'
    if manifest.exists():
        prior = json.loads(manifest.read_text())
        if (prior.get('archive_sha1') == SHA1 and prior.get('directories') == {
                k: list(v) for k, v in DIRECTORIES.items()} and all(
                (OUT / name).is_file() and digest(OUT / name) == sha
                for name, sha in prior['files'].items())):
            return prior
    archive = ROOT / 'android36-16k.zip'
    if not archive.exists():
        print('Downloading fixed Android SDK desktop inputs', flush=True)
        partial = archive.with_suffix('.partial')
        with urllib.request.urlopen(URL, timeout=60) as source, partial.open('wb') as target:
            shutil.copyfileobj(source, target, 4 * 1024 * 1024)
        partial.replace(archive)
    if archive.stat().st_size != SIZE or digest(archive, 'sha1') != SHA1:
        raise RuntimeError('SDK archive checksum mismatch')
    image, partition = ROOT / 'desktop-system-temp.img', ROOT / 'desktop-part-temp.img'
    with zipfile.ZipFile(archive) as package, package.open('arm64-v8a/system.img') as source, image.open('wb') as target:
        shutil.copyfileobj(source, target, 4 * 1024 * 1024)
    with (OUT / 'extract.log').open('w') as log:
        for name, directories in DIRECTORIES.items():
            print(f'Extracting {name}', flush=True)
            system_partition(image, partition, name)
            target = OUT / name
            target.mkdir(exist_ok=True)
            prefix = '/system' if name == 'system' else ''
            for directory in directories:
                subprocess.run([DEBUGFS, '-R', f'rdump {prefix}/{directory} {target}', partition],
                               stdout=log, stderr=log, check=True, timeout=300)
    files = {str(p.relative_to(OUT)): digest(p) for name in DIRECTORIES
             for p in sorted((OUT / name).rglob('*')) if p.is_file() and not p.is_symlink()}
    if not any('Launcher' in p and p.endswith('.apk') for p in files):
        raise RuntimeError('SDK Launcher APK missing')
    if not any('SystemUI' in p and p.endswith('.apk') for p in files):
        raise RuntimeError('SDK SystemUI APK missing')
    if not any(p.startswith('vendor/lib64/egl/') for p in files):
        raise RuntimeError('SDK EGL drivers missing')
    result = dict(url=URL, archive_sha1=SHA1, directories=DIRECTORIES, files=files)
    manifest.write_text(json.dumps(result, indent=2) + '\n')
    for path in (archive, image, partition):
        path.unlink()
    print(f'Verified {len(files)} desktop inputs; intermediate images removed', flush=True)
    return result


def prepare_vendor_module(name):
    """提取已校验 SDK 中的 vendor HAL APEX，保存源归档与文件哈希。"""
    source = OUT / 'vendor/apex' / (name + '.apex')
    target = OUT / 'vendor-apex' / name
    target.mkdir(parents=True, exist_ok=True)
    manifest = target / 'extract-manifest.json'
    source_hash = digest(source)
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        if previous['source_sha256'] == source_hash and all(
                (target / path).is_file() and digest(target / path) == sha
                for path, sha in previous['files'].items()):
            return target
    image = target / 'payload.tmp.img'
    with zipfile.ZipFile(source) as package:
        image.write_bytes(package.read('apex_payload.img'))
    try:
        subprocess.run([DEBUGFS, '-R', f'rdump / {target}', image],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=120)
    finally:
        image.unlink()
    files = {str(p.relative_to(target)): digest(p) for p in sorted(target.rglob('*'))
             if p.is_file() and p != manifest and not p.is_symlink()}
    if not (target / 'apex_manifest.pb').is_file():
        raise RuntimeError('Vendor APEX extraction failed: ' + name)
    manifest.write_text(json.dumps(dict(source_sha256=source_hash, files=files), indent=2) + '\n')
    return target


def prepare_aosp_components():
    """校验并提取 API 36 的桌面、时钟和模块元数据，保留原始签名。"""
    target = ROOT / 'android36-aosp-components'
    entries = (
        ('system_ext', 'priv-app/Launcher3QuickStep/Launcher3QuickStep.apk',
         '6cc60c3bc96317097579445d5b71d7d2604d68a3163025e2f121367ed3dc0f8f'),
        ('product', 'app/ModuleMetadata/ModuleMetadata.apk',
         '6c433bbf5663c51f1082d48252a252b5b4eff265747fca873ac70566f428248d'),
        ('system_ext', 'priv-app/Launcher3QuickStep/oat/arm64/Launcher3QuickStep.vdex',
         '213e1030dd57cc364f454615474b760e4eb9016784ade1bc9677e91c54799e13'),
        ('product', 'app/DeskClock/DeskClock.apk',
         '2213388d2a5c601562de6f6b887d3b849f35562ca98597743316426c7fdc3638'),
        ('product', 'app/DeskClock/oat/arm64/DeskClock.vdex',
         '6b73c635c56793a824490168885a104ca1115668af8686cd9c74327c713aa40e'),
    )
    files = [('/' + part + '/' + path, target / path.split('/', 1)[1])
             for part, path, _ in entries]
    if all(path.is_file() and digest(path) == entry[2] for (_, path), entry in zip(files, entries)) and (target / 'NOTICE.txt').is_file():
        return files
    target.mkdir(exist_ok=True)
    archive = target / 'aosp36.zip'
    image, partition = target / 'system.img', target / 'partition.img'
    try:
        with urllib.request.urlopen('https://dl.google.com/android/repository/sys-img/android/arm64-v8a-36_r02.zip', timeout=60) as source, archive.open('wb') as output:
            shutil.copyfileobj(source, output, 4 * 1024 * 1024)
        if archive.stat().st_size != 810736863 or digest(archive, 'sha1') != '62ad6714df790f89c8a8ad32552ffe20bb16fe87':
            raise RuntimeError('AOSP SDK archive checksum mismatch')
        with zipfile.ZipFile(archive) as package:
            (target / 'NOTICE.txt').write_bytes(package.read('arm64-v8a/NOTICE.txt'))
            with package.open('arm64-v8a/system.img') as source, image.open('wb') as output:
                shutil.copyfileobj(source, output, 4 * 1024 * 1024)
        directories = dict.fromkeys((part, '/'.join(path.split('/')[:2])) for part, path, _ in entries)
        for part, directory in directories:
            system_partition(image, partition, part)
            subprocess.run([DEBUGFS, '-R', f'rdump /{directory} {target}', partition],
                           check=True, capture_output=True, timeout=60)
        if not all(path.is_file() and digest(path) == entry[2] for (_, path), entry in zip(files, entries)):
            raise RuntimeError('AOSP component APK checksum mismatch')
        return files
    finally:
        for path in (archive, image, partition):
            path.unlink(missing_ok=True)


if __name__ == '__main__':
    prepare()
