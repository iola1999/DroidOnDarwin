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


if __name__ == '__main__':
    prepare()
