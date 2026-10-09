#!/usr/bin/env python3
"""构建适用于 NCE 指令改写的 Android BoringSSL 共享库。"""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

from build_android_linker import WORK, TOOL, sha

REVISION = '1e3b727981831886a0910cd9e82d5bbee4cc7029'
ARCHIVE_SHA = 'fd5b10bcb5d55678782a3d518cd68e0d191ac1a3bb6b1a2faebb566af699305a'
OUT = WORK / 'crypto-build'


def build():
    inputs = {'revision': REVISION, 'archive': ARCHIVE_SHA, 'builder': sha(Path(__file__)),
              'ndk': sha(TOOL.parents[3] / 'source.properties')}
    fingerprint = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    OUT.mkdir(exist_ok=True)
    manifest = OUT / 'manifest.json'
    if manifest.exists():
        prior = json.loads(manifest.read_text())
        if prior['build_inputs'] == fingerprint and all((OUT / name).is_file() and sha(OUT / name) == digest
                                                       for name, digest in prior['files'].items()):
            return {name: OUT / name for name in prior['files']}
    archive = WORK / 'boringssl.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve(f'https://android.googlesource.com/platform/external/boringssl/+archive/{REVISION}.tar.gz', archive)
    if sha(archive) != ARCHIVE_SHA:
        raise RuntimeError('BoringSSL source checksum mismatch')
    source = WORK / 'boringssl'
    source.mkdir(exist_ok=True)
    with tarfile.open(archive) as package:
        for member in package:
            if not member.issym():
                package.extract(member, source, filter='data')
    flags = '-ffixed-x18 -mno-outline-atomics -DBORINGSSL_ANDROID_SYSTEM -DOPENSSL_SMALL'
    command = ['cmake', '-S', str(source / 'src'), '-B', str(OUT),
               '-DCMAKE_TOOLCHAIN_FILE=' + str(TOOL.parents[3] / 'build/cmake/android.toolchain.cmake'),
               '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-35', '-DANDROID_STL=c++_static',
               '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON', '-DBUILD_TESTING=OFF', '-DFIPS=OFF',
               '-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags,
               '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-Bsymbolic,-z,max-page-size=16384,-z,common-page-size=16384']
    with (OUT / 'build.log').open('w') as log:
        subprocess.run(command, check=True, timeout=120, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run(['cmake', '--build', str(OUT), '--target', 'crypto', 'ssl', '-j8'],
                       check=True, timeout=300, stdout=log, stderr=subprocess.STDOUT)
    for name in ('libcrypto.so', 'libssl.so'):
        subprocess.run([TOOL / 'bin/llvm-strip', '--strip-debug', OUT / name], check=True, timeout=30)
    files = {name: sha(OUT / name) for name in ('libcrypto.so', 'libssl.so')}
    manifest.write_text(json.dumps({'build_inputs': fingerprint, 'inputs': inputs, 'fips': False,
                                   'assembly': True, 'files': files}, indent=2) + '\n')
    return {name: OUT / name for name in files}


if __name__ == '__main__':
    print(json.dumps({name: str(path) for name, path in build().items()}))
