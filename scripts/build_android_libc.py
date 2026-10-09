#!/usr/bin/env python3
"""为 NCE 构建关闭 TBI 的 Bionic 共享库。"""
import concurrent.futures
import hashlib
import json
import shutil
import subprocess
import tarfile
import urllib.request
from pathlib import Path

from build_android_linker import WORK, TOOL, SOURCES, build as build_linker, sha


def build():
    linker = build_linker()
    out = WORK / 'libc-build'
    out.mkdir(exist_ok=True)
    library = TOOL / 'sysroot/usr/lib/aarch64-linux-android'
    unwind_archive = WORK / 'libunwind.tar.xz'
    if not unwind_archive.exists():
        urllib.request.urlretrieve('https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/libunwind-20.1.8.src.tar.xz', unwind_archive)
    if sha(unwind_archive) != '0bced9d701e300f8fe6599523367e214c1f928ac559afceece58f47018e9c4a7':
        raise RuntimeError('libunwind source checksum mismatch')
    unwind = WORK / 'libunwind-20.1.8.src'
    if not unwind.exists():
        with tarfile.open(unwind_archive) as package:
            package.extractall(WORK, filter='data')
    inputs = {str(path.relative_to(WORK.parent)) if path.is_relative_to(WORK.parent) else str(path): sha(path)
              for path in (library / 'libc.a', linker, unwind_archive,
                           linker.parent / 'libdl.so',
                           WORK.parent / 'compiler-rt-20.1.8.src.tar.xz')}
    fingerprint = hashlib.sha256(Path(__file__).read_bytes() + SOURCES.read_bytes() +
                                 json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    manifest, binary = out / 'manifest.json', out / 'libc.so'
    if manifest.exists() and binary.exists():
        previous = json.loads(manifest.read_text())
        if previous['build_inputs'] == fingerprint and previous['sha256'] == sha(binary):
            return binary
    bionic = WORK / 'bionic-source/libc'
    # Keep the 16 KiB SCS and its guard page while fitting many threads in the
    # guest's 4 GiB address window. pthread_create and pthread_exit must agree.
    scs_config = out / 'nce-scs.h'
    scs_config.write_text(f'#include "{bionic / "private/bionic_constants.h"}"\n'
                          '#undef SCS_GUARD_REGION_SIZE\n'
                          '#define SCS_GUARD_REGION_SIZE (1024 * 1024)\n')
    scudo = WORK / 'scudo'
    configuration = (scudo / 'config/custom_scudo_config.h').read_text()
    selector = 'typedef AndroidNormalConfig Config;'
    if configuration.count(selector) != 1:
        raise RuntimeError('Scudo Android configuration selector changed')
    configuration = configuration.replace(selector, '''
// iOS has a bounded guest address window. Reserve size classes on first use;
// exhausted classes retain Scudo's larger-class and secondary allocation path.
struct NceConfig : AndroidNormalConfig {
  struct Primary : AndroidNormalConfig::Primary {
    static const uptr RegionSizeLog = 25U;
    static const bool EnableContiguousRegions = false;
  };
};
typedef NceConfig Config;''')
    (out / 'custom_scudo_config.h').write_text(configuration)
    compiler_rt = WORK.parent / 'compiler-rt-20.1.8.src/lib'
    includes = [out, bionic, bionic / 'platform', bionic / 'async_safe/include',
                WORK / 'core-include', WORK / 'liblog/liblog/include',
                scudo / 'standalone/include', scudo / 'config', compiler_rt,
                bionic.parent / 'libdl/include_private']
    flags = ['--target=aarch64-linux-android10000', '-O2', '-g', '-fPIC', '-ftrivial-auto-var-init=zero', '-ffixed-x18',
             '-mno-outline-atomics', '-march=armv8-a+crc', '-fno-stack-protector', '-fno-builtin',
             '-ffunction-sections', '-fdata-sections', '-nostdinc++', '-DUSE_SCUDO',
             '-D__BIONIC_BUILD_FOR_ANDROID_SUPPORT', '-D__ANDROID_APEX__',
             '-DSCUDO_USE_CUSTOM_CONFIG', '-DSCUDO_DISABLE_TBI', '-D_BIONIC=1',
             '-DSCUDO_HAS_PLATFORM_TLS_SLOT', '-DSCUDO_MIN_ALIGNMENT_LOG=4',
             '-DHAVE_ANDROID_UNSAFE_FRAME_POINTER_CHASE',
             *[value for path in includes for value in ('-I', str(path))]]
    scudo_names = ['checksum', 'common', 'condition_variable_linux', 'flags', 'flags_parser',
                   'linux', 'mem_map', 'mem_map_linux', 'release', 'report', 'report_linux',
                   'string_utils', 'timing', 'crc32_hw', 'wrappers_c_bionic']
    shared_names = ['libc_init_dynamic', 'android_mallopt', 'gwp_asan_wrappers', 'heap_tagging',
                    'malloc_common', 'malloc_common_dynamic', 'android_profiling_dynamic',
                    'malloc_heapprofd', 'malloc_limit', 'ndk_cruft', 'ndk_cruft_data', 'NetdClient',
                    'pthread_create', 'pthread_exit']
    units = [scudo / 'standalone' / (name + '.cpp') for name in scudo_names]
    units += [bionic / 'bionic' / (name + '.cpp') for name in shared_names]
    units += [bionic / 'arch-common/bionic/crtbegin_so.c', bionic / 'arch-common/bionic/crtend_so.S']
    units += [bionic.parent / 'libdl/libdl_android.cpp']

    def compile(source):
        obj = out / (str(source.relative_to(WORK)).replace('/', '_') + '.o')
        cpp = source.suffix == '.cpp'
        extra = ['-std=gnu++20', '-fno-exceptions', '-fno-rtti'] if cpp else []
        if source.stem in ('pthread_create', 'pthread_exit'):
            extra += ['-include', str(scs_config)]
        result = subprocess.run([TOOL / 'bin' / ('clang++' if cpp else 'clang'), *flags, *extra,
                                 '-c', source, '-o', obj], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(f'{source.name}:\n{result.stderr}')
        return obj

    with concurrent.futures.ThreadPoolExecutor(6) as pool:
        objects = list(pool.map(compile, units))
    for name in ('libunwind.cpp', 'UnwindLevel1.c', 'UnwindLevel1-gcc-ext.c',
                 'UnwindRegistersRestore.S', 'UnwindRegistersSave.S'):
        source, obj = unwind / 'src' / name, out / ('unwind-' + name + '.o')
        cpp = source.suffix == '.cpp'
        unwind_flags = [f for f in flags if f != '-nostdinc++']
        subprocess.run([TOOL / 'bin' / ('clang++' if cpp else 'clang'), *unwind_flags,
                        '-I', unwind / 'include', '-D_LIBUNWIND_IS_NATIVE_ONLY',
                        *(['-std=gnu++20', '-fno-exceptions', '-fno-rtti'] if cpp else []),
                        '-c', source, '-o', obj], check=True, timeout=120)
        objects.append(obj)
    # NDK 的归档保留了通用 Bionic 对象；共享库启动和分配器按源码重新编译。
    archive = out / 'libc-common.a'
    shutil.copyfile(library / 'libc.a', archive)
    removed = {name + '.o' for name in scudo_names + shared_names}
    removed |= {'libc_init_static.o', 'dl_iterate_phdr_static.o'}
    subprocess.run([TOOL / 'bin/llvm-ar', 'd', archive, *sorted(removed)], check=True)
    lines = []
    for line in (bionic / 'libc.map.txt').read_text().splitlines():
        tags = line.partition('#')[2].split()
        arches = set(tags) & {'arm', 'arm64', 'x86', 'x86_64', 'riscv64'}
        if not arches or 'arm64' in arches:
            lines.append(line)
    version_map = out / 'libc.arm64.map'
    version_map.write_text('\n'.join(lines) + '\n')
    command = [TOOL / 'bin/aarch64-linux-android35-clang++', '-nostdlib', '-shared',
               '-Wl,-soname,libc.so,-Bsymbolic-functions,-z,max-page-size=16384,-z,common-page-size=16384',
               '-Wl,-z,defs,--gc-sections,--version-script=' + str(version_map), *objects,
               '-Wl,--whole-archive', archive, '-Wl,--no-whole-archive',
               linker.parent / 'libdl.so', linker,
               TOOL / 'lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a', '-o', binary]
    subprocess.run(command, check=True, timeout=120)
    subprocess.run([TOOL / 'bin/llvm-strip', '--strip-debug', binary], check=True)
    manifest.write_text(json.dumps(dict(build_inputs=fingerprint, sha256=sha(binary),
                                       inputs=inputs, scudo_disable_tbi=True,
                                       scudo_region_bytes=1 << 25,
                                       scs_guard_region_bytes=1 << 20,
                                       scudo_contiguous_regions=False), indent=2) + '\n')
    return binary


if __name__ == '__main__':
    print(build())
