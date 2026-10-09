#!/usr/bin/env python3
"""提取固定 SDK 的 APEX 原始归档、模块元数据和分区 aconfig 文件。"""
import json
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request
import zipfile
from prepare_android_runtime import ROOT, DEST, URL, SHA1, SIZE, APEX, DEBUGFS, digest, system_partition

SOURCE = Path(__file__).resolve().parents[1] / 'experiments/native-linux/services/apex'
OUT = ROOT / 'android36-modules'


def prepare():
    manifest = OUT / 'manifest.json'
    schema_hash = digest(SOURCE / 'apex_manifest.proto')
    if manifest.exists():
        prior = json.loads(manifest.read_text())
        if (prior['archive_sha1'] == SHA1 and prior['schema_sha256'] == schema_hash
                and [m['source'] for m in prior['modules']] == list(APEX) and all(
                (OUT / name).is_file() and digest(OUT / name) == sha for name, sha in prior['files'].items())):
            return prior
    OUT.mkdir(exist_ok=True)
    (OUT / 'archives').mkdir(exist_ok=True)
    archive = ROOT / 'android36-16k.zip'
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=60) as source, archive.open('wb') as target:
            shutil.copyfileobj(source, target, 4 * 1024 * 1024)
    if archive.stat().st_size != SIZE or digest(archive, 'sha1') != SHA1:
        raise RuntimeError('SDK archive checksum mismatch')
    image, partition = ROOT / 'apex-system-temp.img', ROOT / 'apex-part-temp.img'
    with zipfile.ZipFile(archive) as package, package.open('arm64-v8a/system.img') as source, image.open('wb') as target:
        shutil.copyfileobj(source, target, 4 * 1024 * 1024)
    modules = []
    with (OUT / 'extract.log').open('w') as log:
        def debug(command):
            subprocess.run([DEBUGFS, '-R', command, partition], stdout=log, stderr=log, check=True, timeout=120)
        system_partition(image, partition)
        for name in APEX:
            temporary = ROOT / 'apex-input-temp.zip'
            debug(f'dump /system/apex/{name} {temporary}')
            if name.endswith('.capex'):
                with zipfile.ZipFile(temporary) as package:
                    content = package.read('original_apex')
                temporary.write_bytes(content)
            with zipfile.ZipFile(temporary) as package:
                pb = package.read('apex_manifest.pb')
            decoded = subprocess.check_output(['protoc', f'--proto_path={SOURCE}',
                       '--decode=apex.proto.ApexManifest', SOURCE / 'apex_manifest.proto'], input=pb).decode()
            module = json.loads(re.search(r'^name: (".*")$', decoded, re.M)[1])
            version = int(re.search(r'^version: (\d+)$', decoded, re.M)[1])
            version_name = re.search(r'^versionName: (".*")$', decoded, re.M)
            if not re.fullmatch(r'com\.android\.[a-z0-9.]+', module) or version <= 0:
                raise RuntimeError('Unexpected APEX identity')
            if (DEST / module / 'apex_manifest.pb').read_bytes() != pb:
                raise RuntimeError(f'Expanded APEX does not match its archive: {module}')
            target = OUT / 'archives' / (module + '.apex')
            temporary.replace(target)
            modules.append(dict(name=module, version=version,
                                version_name=json.loads(version_name[1]) if version_name else '',
                                source=name, archive=str(target.relative_to(OUT))))
        for name in ('system_ext', 'product', 'vendor'):
            system_partition(image, partition, name)
            target = OUT / 'config' / name
            target.mkdir(parents=True, exist_ok=True)
            for field in ('package.map', 'flag.map', 'flag.val', 'flag.info'):
                debug(f'dump /etc/aconfig/{field} {target / field}')
                if not (target / field).is_file() or not (target / field).stat().st_size:
                    raise RuntimeError(f'Missing {name} aconfig {field}')
    files = {str(p.relative_to(OUT)): digest(p) for directory in ('archives', 'config')
             for p in sorted((OUT / directory).rglob('*')) if p.is_file()}
    result = dict(archive_sha1=SHA1, schema_sha256=schema_hash, modules=modules, files=files)
    manifest.write_text(json.dumps(result, indent=2) + '\n')
    for p in (archive, image, partition, ROOT / 'apex-inspect.capex', ROOT / 'apex-inspect.apex'):
        p.unlink(missing_ok=True)
    return result


if __name__ == '__main__':
    print(json.dumps(prepare(), indent=2))
