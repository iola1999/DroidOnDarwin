#!/usr/bin/env python3
"""构建使用 Linux memfd 的 Android allocator 和 mapper。"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

from build_android_linker import ROOT, WORK as ANDROID, TOOL, sha

WORK = ROOT / 'work/native-linux-nce'
SOURCE = WORK / 'graphics-source'
OUT = WORK / 'gralloc-build'
MANIFEST = ROOT / 'experiments/native-linux/graphics/android-sources.json'


def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, timeout=120, **kwargs)


def prepare_sources():
    SOURCE.mkdir(exist_ok=True)
    for item in json.loads(MANIFEST.read_text())['archives']:
        archive = SOURCE / (item['name'] + '.tar.gz')
        if not archive.exists():
            url = f"https://android.googlesource.com/{item['repo']}/+archive/{item['revision']}/{item['path']}.tar.gz"
            archive.write_bytes(urllib.request.urlopen(url, timeout=40).read())
        target = SOURCE / item['name']
        if target.exists():
            shutil.rmtree(target)
        target.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(target, filter='data')
        tree_hash = hashlib.sha256()
        for path in sorted(target.rglob('*')):
            if path.is_symlink():
                content = b'link:' + os.readlink(path).encode()
            elif path.is_file():
                content = path.read_bytes()
            else:
                continue
            tree_hash.update(str(path.relative_to(target)).encode() + b'\0' + hashlib.sha256(content).digest())
        if tree_hash.hexdigest() != item['tree_sha256']:
            raise RuntimeError('Graphics source tree mismatch: ' + item['name'])


def build():
    OUT.mkdir(exist_ok=True)
    prepare_sources()
    patch = ROOT / 'patches/nce-gralloc-memfd.patch'
    gralloc = OUT / 'source'
    if gralloc.exists():
        shutil.rmtree(gralloc)
    shutil.copytree(SOURCE / 'goldfish-hals/gralloc', gralloc)
    run(['git', 'apply', '--unsafe-paths', patch], cwd=gralloc)
    aidl = Path('/opt/homebrew/share/android-commandlinetools/build-tools/36.0.0/aidl')
    generated = OUT / 'aidl'
    if generated.exists():
        shutil.rmtree(generated)
    generated.mkdir()
    interfaces = SOURCE / 'graphics-interfaces'
    common = SOURCE / 'common-interfaces'
    definitions = [
        (common / 'aidl/aidl_api/android.hardware.common/2', 2),
        (interfaces / 'common/aidl/aidl_api/android.hardware.graphics.common/6', 6),
        (interfaces / 'allocator/aidl/aidl_api/android.hardware.graphics.allocator/2', 2),
    ]
    for directory, version in definitions:
        api_hash = (directory / '.hash').read_text().splitlines()[-1]
        for source in sorted(directory.rglob('*.aidl')):
            run([aidl, '--lang=ndk', '--structured', '--stability=vintf', '--min_sdk_version=35',
                 '--version=' + str(version), '--hash=' + api_hash,
                 *['-I' + str(d) for d, _ in definitions],
                 '--out=' + str(generated), '--header_out=' + str(generated), source])
    guest = SOURCE / 'gfxstream-guest'
    platform_headers = OUT / 'platform-include'
    platform_headers.mkdir(exist_ok=True)
    for name in ('hardware', 'system'):
        shutil.copytree(ROOT / 'work/gfxstream-src/third_party/android/include' / name,
                        platform_headers / name, dirs_exist_ok=True)
    includes = [generated, gralloc, ANDROID / 'binder-include', ANDROID / 'libbase/include',
        ANDROID / 'liblog/liblog/include', common / 'support/include', SOURCE / 'goldfish-debug/include',
        SOURCE / 'cutils-include', SOURCE / 'sync-include',
        interfaces / 'mapper/stable-c/include', interfaces / 'mapper/stable-c/implutils/include',
        guest / 'OpenglSystemCommon', guest / 'renderControl_enc', guest / 'OpenglCodecCommon',
        guest / 'OpenglCodecCommon/include', guest / 'gralloc_cb/include',
        guest / 'qemupipe/include', guest / 'qemupipe/include-types',
        SOURCE / 'mesa-platform/include', SOURCE / 'mesa-android/include', SOURCE / 'mesa-iostream/include',
        platform_headers,
        WORK / 'processes/linux-uapi/include/drm']
    flags = ['--target=aarch64-linux-android35', '-std=c++20', '-O2', '-g', '-fPIC', '-ffixed-x18', '-mno-outline-atomics', '-fno-rtti',
        '-DANDROID', '-D__ANDROID_API_U__=34', '-D__ANDROID_API_V__=35',
        '-DANDROID_BASE_UNIQUE_FD_DISABLE_IMPLICIT_CONVERSION', '-nostdlib++',
        '-include', ANDROID / 'art-build/platform-libcxx-config.h',
        '-Wl,-z,defs,-z,max-page-size=16384,-z,common-page-size=16384',
        *[value for path in includes for value in ('-I', str(path))]]
    system = ANDROID / 'android36/lib64'
    vendor = ANDROID / 'android36-desktop/vendor/lib64'
    libraries = ['android.hardware.graphics.allocator-V2-ndk.so', 'android.hardware.common-V2-ndk.so',
        'libbinder_ndk.so', 'libbase.so', 'libcutils.so', 'liblog.so', 'libutils.so',
        'libOpenglCodecCommon.so', 'libOpenglSystemCommon.so', 'lib_renderControl_enc.so', 'libc++.so']
    library_paths = [next(d / name for d in (system, vendor) if (d / name).exists()) for name in libraries]
    binaries = [OUT / 'android.hardware.graphics.allocator-service.nce', OUT / 'mapper.ranchu.so']
    with (OUT / 'build.log').open('w') as log:
        run([TOOL / 'bin/clang++', *flags, '-DLOG_TAG="allocator-service.nce"',
             gralloc / 'allocator.cpp', common / 'support/NativeHandle.cpp', *library_paths,
             vendor / 'libqemupipe.ranchu.so', '-o', binaries[0]], stdout=log, stderr=subprocess.STDOUT)
        run([TOOL / 'bin/clang++', *flags, '-DLOG_TAG="mapper.nce"', '-shared',
             gralloc / 'mapper.cpp', *library_paths, system / 'libsync.so', vendor / 'libandroidemu.so',
             '-o', binaries[1]], stdout=log, stderr=subprocess.STDOUT)
    result = dict(inputs={str(p.relative_to(ROOT)): sha(p) for p in (Path(__file__), MANIFEST, patch)},
                  libraries={str(p): sha(p) for p in library_paths},
                  binaries={str(p): sha(p) for p in binaries})
    (OUT / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    return binaries


if __name__ == '__main__':
    for path in build():
        print(path)
