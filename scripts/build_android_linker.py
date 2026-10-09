#!/usr/bin/env python3
"""构建使用 NCE ELF 地址预留接口的 Android 动态链接器。"""
import base64
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce/android'
NDK = Path('/opt/homebrew/share/android-ndk')
TOOL = NDK / 'toolchains/llvm/prebuilt/darwin-x86_64'
SOURCES = ROOT / 'experiments/native-linux/android-sources.json'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def build():
    if 'Pkg.Revision = 29.0.14206865' not in (NDK / 'source.properties').read_text():
        raise RuntimeError('Linker build requires Android NDK 29.0.14206865')
    WORK.mkdir(parents=True, exist_ok=True)
    output = WORK / 'linker-build'
    output.mkdir(exist_ok=True)
    library = TOOL / 'sysroot/usr/lib/aarch64-linux-android'
    ndk_inputs = {name: sha(library / name) for name in
                  ('libc++_static.a', 'libc++abi.a', 'libc.a', 'libm.a', 'libdl.a', 'libz.a')}
    fingerprint = hashlib.sha256(Path(__file__).read_bytes() + SOURCES.read_bytes() +
                                 json.dumps(ndk_inputs, sort_keys=True).encode()).hexdigest()
    manifest = output / 'manifest.json'
    binary = output / 'linker64'
    if manifest.exists() and binary.exists():
        previous = json.loads(manifest.read_text())
        if previous['build_inputs'] == fingerprint and previous['sha256'] == sha(binary):
            return binary
    sources = json.loads(SOURCES.read_text())
    for item in sources['archives']:
        archive = WORK / item.get('archive', item['name'] + '.tar.gz')
        if not archive.exists():
            url = f"https://android.googlesource.com/{item['repo']}/+archive/{item['revision']}.tar.gz"
            archive.write_bytes(urllib.request.urlopen(url, timeout=60).read())
        if sha(archive) != item['sha256']:
            raise RuntimeError(f"Source checksum mismatch: {item['name']}")
        destination = WORK / item['name']
        destination.mkdir(exist_ok=True)
        with tarfile.open(archive) as package:
            for member in package:
                # Repository tooling symlinks can point outside these snapshots.
                if not member.issym():
                    package.extract(member, destination, filter='data')
    core = WORK / 'core-include/private/android_filesystem_config.h'
    core.parent.mkdir(parents=True, exist_ok=True)
    if not core.exists():
        core.write_bytes(base64.b64decode(urllib.request.urlopen(sources['core_header']['url'], timeout=30).read()))
    if sha(core) != sources['core_header']['sha256']:
        raise RuntimeError('Core header checksum mismatch')
    bionic = WORK / 'bionic-source'
    phdr = bionic / 'linker/linker_phdr.cpp'
    original = ('start = ReserveWithAlignmentPadding(load_size_, kLibraryAlignment, start_alignment, &gap_start_,\n'
                '                                        &gap_size_);\n    if (start == nullptr)')
    replacement = ('start = reinterpret_cast<void*>(syscall(245, fd_, load_size_));\n'
                   '    gap_start_ = nullptr;\n    gap_size_ = 0;\n    if (start == MAP_FAILED)')
    text = phdr.read_text()
    if text.count(original) != 1:
        raise RuntimeError('Android linker reservation code changed')
    phdr.write_text(text.replace(original, replacement))
    includes = [bionic / 'libc', bionic / 'libc/platform', bionic / 'libc/async_safe/include',
                WORK / 'libbase/include', WORK / 'libziparchive/include',
                WORK / 'libziparchive/incfs_support/include', WORK / 'liblog/liblog/include', WORK / 'core-include']
    flags = ['--target=aarch64-linux-android10000', '-O2', '-g', '-fPIC', '-ffixed-x18', '-mno-outline-atomics',
             '-fno-stack-protector', '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections',
             '-D__BIONIC_BUILD_FOR_ANDROID_SUPPORT', '-D__ANDROID_APEX__', '-DZLIB_CONST',
             '-DINCFS_SUPPORT_DISABLED=1', '-DANDROID_DEBUGGABLE=1', '-DSNET_EVENT_LOG_TAG=0x534e4554',
             '-DLIBLOG_LOG_TAG=1006', *[value for path in includes for value in ('-I', str(path))]]
    bp = (bionic / 'linker/Android.bp').read_text()
    group = re.search(r'name: "linker_sources",\s*srcs: \[(.*?)\]', bp, re.S)[1]
    units = [bionic / 'linker' / name for name in re.findall(r'"([^"]+)"', group)]
    units += [bionic / 'linker' / name for name in ('linker_main.cpp', 'linker_memory.cpp',
              'linker_debuggerd_stub.cpp', 'linker_translate_path.cpp', 'arch/arm64/begin.S',
              'arch/arm64/tlsdesc_resolver.S', 'arch/arm_neon/linker_gnu_hash_neon.cpp')]
    units += [bionic / 'libc/async_safe/async_safe_log.cpp']
    units += [WORK / 'libbase' / name for name in ('file.cpp', 'logging.cpp', 'strings.cpp',
              'stringprintf.cpp', 'properties.cpp', 'parsebool.cpp', 'posix_strerror_r.cpp',
              'errors_unix.cpp', 'mapped_file.cpp')]
    units += [WORK / 'libziparchive' / name for name in ('zip_archive.cc', 'zip_archive_stream_entry.cc',
              'zip_cd_entry_map.cc', 'zip_error.cpp')]
    units += [WORK / 'liblog/liblog' / name for name in ('logger_write.cpp', 'logger_name.cpp',
              'properties.cpp', 'logd_writer.cpp', 'pmsg_writer.cpp', 'log_event_write.cpp', 'log_event_list.cpp')]

    def compile(source):
        obj = output / (str(source.relative_to(WORK)).replace('/', '_') + '.o')
        cpp = source.suffix in ('.cpp', '.cc')
        extra = ['-std=gnu++20', '-fno-exceptions', '-fno-rtti'] if cpp else ['-D__ASSEMBLY__'] if source.suffix == '.S' else []
        result = subprocess.run([TOOL / 'bin' / ('clang++' if cpp else 'clang'), *flags, *extra,
                                 '-c', source, '-o', obj], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(f'{source.name}:\n{result.stderr}')
        return obj

    with concurrent.futures.ThreadPoolExecutor(6) as pool:
        objects = list(pool.map(compile, units))
    library = TOOL / 'sysroot/usr/lib/aarch64-linux-android'
    command = [TOOL / 'bin/aarch64-linux-android35-clang++', '-nostdlib', '-shared',
               '-Wl,-Bsymbolic,--exclude-libs,ALL,-soname,ld-android.so,--pack-dyn-relocs=relr',
               '-Wl,-z,max-page-size=16384,-z,common-page-size=16384,--gc-sections,-z,defs,--entry=_start',
               '-Wl,--version-script=' + str(bionic / 'linker/linker.generic.map'), *objects,
               '-Wl,--start-group', *[library / name for name in ('libc++_static.a', 'libc++abi.a', 'libc.a', 'libm.a', 'libdl.a', 'libz.a')],
               TOOL / 'lib/clang/21/lib/linux/aarch64/libunwind.a', '-Wl,--end-group',
               TOOL / 'lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a', '-o', binary]
    subprocess.run(command, check=True, timeout=120)
    subprocess.run([TOOL / 'bin/llvm-strip', '--strip-debug', binary], check=True, timeout=30)
    manifest.write_text(json.dumps(dict(build_inputs=fingerprint, sha256=sha(binary),
                                       sources=sources, ndk_inputs=ndk_inputs, reservation_syscall=245), indent=2) + '\n')
    return binary


if __name__ == '__main__':
    print(build())
