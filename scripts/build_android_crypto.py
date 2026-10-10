#!/usr/bin/env python3
"""构建适用于 NCE 指令改写的 Android BoringSSL 共享库。"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

from build_android_linker import WORK, TOOL, sha

REVISION = 'ecc1358826150d6a1851c517c325b0e6c0e1b8be'
ARCHIVE_SHA = 'e1208516f7419fdcb4ebcb8d0afd7153d0abba7d4eee52bc57a5136cb1263557'
OUT = WORK / 'crypto-build'
CRONET_REVISION = '3d00a6f18075427606068c038ea2695053213440'
CRONET_ARCHIVE_SHA = 'a8d924f47399862907a59125c6c61a9b5f3702da5b0bc9f31d929c9e90be78c6'


def build(cronet=False):
    revision = CRONET_REVISION if cronet else REVISION
    archive_sha = CRONET_ARCHIVE_SHA if cronet else ARCHIVE_SHA
    out = WORK / 'cronet-crypto-build' if cronet else OUT
    source_name = 'cronet-boringssl' if cronet else 'boringssl'
    names = ('stable_cronet_libcrypto.so', 'stable_cronet_libssl.so') if cronet else ('libcrypto.so', 'libssl.so')
    url = (f'https://android.googlesource.com/platform/external/cronet/+archive/{revision}/stable/third_party/boringssl.tar.gz'
           if cronet else f'https://android.googlesource.com/platform/external/boringssl/+archive/{revision}.tar.gz')
    inputs = {'revision': revision, 'archive': archive_sha, 'cronet': cronet, 'builder': sha(Path(__file__)),
              'ndk': sha(TOOL.parents[3] / 'source.properties')}
    fingerprint = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    out.mkdir(exist_ok=True)
    manifest = out / 'manifest.json'
    if manifest.exists():
        prior = json.loads(manifest.read_text())
        if prior['build_inputs'] == fingerprint and all((out / name).is_file() and sha(out / name) == digest
                                                       for name, digest in prior['files'].items()):
            return {name: out / name for name in prior['files']}
    archive = WORK / (source_name + '-' + revision + '.tar.gz')
    if not archive.exists():
        urllib.request.urlretrieve(url, archive)
    if sha(archive) != archive_sha:
        raise RuntimeError('BoringSSL source checksum mismatch')
    source = WORK / source_name
    revision_file = source / '.nce-source-revision'
    if source.exists() and (not revision_file.exists() or revision_file.read_text().strip() != revision):
        shutil.rmtree(source)
    source.mkdir(exist_ok=True)
    with tarfile.open(archive) as package:
        for member in package:
            if not member.issym():
                package.extract(member, source, filter='data')
    revision_file.write_text(revision + '\n')
    # Android's archive omits the upstream test/PKI submodules. Keep the
    # original crypto/ssl build rules and omit unrelated CMake targets.
    cmake = source / 'src/CMakeLists.txt'
    text = cmake.read_text()
    first = text.index('# Add minimal googletest targets.')
    end = text.index('if(OPENSSL_ASM)', first)
    text = text[:first] + text[end:]
    text = text[:text.index('\nadd_library(decrepit')]
    if cronet:
        text += '\nset_target_properties(crypto PROPERTIES PREFIX "" OUTPUT_NAME "stable_cronet_libcrypto")\n'
        text += 'set_target_properties(ssl PROPERTIES PREFIX "" OUTPUT_NAME "stable_cronet_libssl")\n'
    cmake.write_text(text)
    flags = '-ffixed-x18 -mno-outline-atomics -DBORINGSSL_ANDROID_SYSTEM -DOPENSSL_SMALL'
    command = ['cmake', '-S', str(source / 'src'), '-B', str(out),
               '-DCMAKE_TOOLCHAIN_FILE=' + str(TOOL.parents[3] / 'build/cmake/android.toolchain.cmake'),
               '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-35', '-DANDROID_STL=c++_static',
               '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON', '-DBUILD_TESTING=OFF', '-DFIPS=OFF',
               '-DCMAKE_C_FLAGS=' + flags, '-DCMAKE_CXX_FLAGS=' + flags,
               '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-Bsymbolic,-z,max-page-size=16384,-z,common-page-size=16384']
    with (out / 'build.log').open('w') as log:
        subprocess.run(command, check=True, timeout=120, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run(['cmake', '--build', str(out), '--target', 'crypto', 'ssl', '-j8'],
                       check=True, timeout=300, stdout=log, stderr=subprocess.STDOUT)
    for name in names:
        subprocess.run([TOOL / 'bin/llvm-strip', '--strip-debug', out / name], check=True, timeout=30)
    files = {name: sha(out / name) for name in names}
    manifest.write_text(json.dumps({'build_inputs': fingerprint, 'inputs': inputs, 'fips': False,
                                   'assembly': True, 'files': files}, indent=2) + '\n')
    return {name: out / name for name in files}


if __name__ == '__main__':
    print(json.dumps({name: str(path) for name, path in build().items()}))
