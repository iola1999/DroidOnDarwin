#!/usr/bin/env python3
"""构建使用高地址对象堆的 Android 16 ART 与 ARM64 JIT。"""
import base64
import concurrent.futures
import functools
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import urllib.request

from build_android_libc import build as build_libc
from build_android_linker import ROOT, TOOL, WORK, sha

PATCHES = ROOT / 'experiments/native-linux/art'
OUT = WORK / 'art-build'


def source_list(directory, module):
    content = (directory / 'Android.bp').read_text().split(f'name: "{module}"', 1)[1]
    content = re.sub(r'//[^\n]*', '', content)
    return [directory / name for name in re.findall(r'"([^"\n]+)"',
            re.search(r'\bsrcs:\s*\[(.*?)\]', content, re.S)[1])]


def restore_sources():
    sources = json.loads((PATCHES / 'sources.json').read_text())
    for item in sources['archives']:
        archive = WORK / (item['name'] + '.tar.gz')
        if not archive.exists():
            suffix = '/' + item['subtree'] if item.get('subtree') else ''
            url = f"https://android.googlesource.com/{item['repo']}/+archive/{item['revision']}{suffix}.tar.gz"
            archive.write_bytes(urllib.request.urlopen(url, timeout=60).read())
        if sha(archive) != item['sha256']:
            raise RuntimeError(f"ART source checksum mismatch: {item['name']}")
        destination = WORK / item['name']
        destination.mkdir(exist_ok=True)
        with tarfile.open(archive) as package:
            for member in package:
                if not member.issym():
                    package.extract(member, destination, filter='data')
    for item in sources['headers']:
        path = WORK / item['path']
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(base64.b64decode(urllib.request.urlopen(item['url'], timeout=30).read()))
        if sha(path) != item['sha256']:
            raise RuntimeError(f"ART header checksum mismatch: {item['path']}")
    subprocess.run(['patch', '--posix', '--batch', '-p1', '-i', str(PATCHES / 'android16.patch')],
                   cwd=WORK / 'art', check=True, capture_output=True)
    for name in ('nterp.patch', 'compiler-profile.patch', 'expanded-jit.patch', 'fast-baseline.patch', 'offline-compiler.patch', 'relocatable-code.patch'):
        subprocess.run(['patch', '--posix', '--batch', '-p1', '-i', str(PATCHES / name)],
                       cwd=WORK / 'art', check=True, capture_output=True)


def build(include_compiler=False):
    libc = build_libc()
    compiler_dependencies = {}
    if include_compiler:
        from build_android_crypto import build as build_crypto
        crypto = build_crypto()
        compiler_dependencies['libcrypto.so'] = sha(crypto['libcrypto.so'])
    OUT.mkdir(exist_ok=True)
    config = TOOL / 'sysroot/usr/include/c++/v1/__config_site'
    inputs = {str(path.relative_to(ROOT)): sha(path) for path in [PATCHES / name for name in ('android16.patch', 'reference_heap.h',
                  'reference_heap.c', 'jit_control.cc', 'sources.json', 'nterp.patch', 'compiler-profile.patch', 'expanded-jit.patch', 'fast-baseline.patch', 'offline-compiler.patch', 'relocatable-code.patch', 'dex2oat-main.cc')]
              if path.is_file()}
    inputs.update(ndk_libcxx_config=sha(config), libc=sha(libc),
                  runtime=sha(WORK / 'android36-runtime.json'), builder=sha(Path(__file__)),
                  layout=sha(ROOT / 'experiments/native-linux/layout.h'), offline_compiler=include_compiler)
    fingerprint = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    manifest = OUT / 'manifest.json'
    required = ['libart.so', 'libartbase.so'] + (['nce-dex2oat'] if include_compiler else [])
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        if (previous['build_inputs'] == fingerprint and
                (not include_compiler or previous.get('compiler_dependencies') == compiler_dependencies) and
                all(name in previous['files'] and
                (OUT / name).exists() and sha(OUT / name) == previous['files'][name] for name in required)):
            return {name: OUT / name for name in required}
    restore_sources()
    art = WORK / 'art'
    # 平台共享库使用 __1；NDK 头文件的默认命名空间是 __ndk1。
    platform_config = OUT / 'platform-libcxx-config.h'
    platform_config.write_text(config.read_text().replace('__ndk1', '__1'))
    includes = [art / name for name in ('runtime', 'libartbase', 'libdexfile', 'libprofile',
                'libartpalette/include', 'libnativebridge/include', 'libnativeloader/include',
                'sigchainlib', 'cmdline', 'tools/cpp-define-generator', 'compiler',
                'compiler/export', 'libelffile', 'odrefresh/include', 'dex2oat')]
    includes += [WORK / name for name in ('bionic-source/libc/platform', 'libbase/include',
                  'liblog/liblog/include', 'libziparchive/include', 'fmt/include', 'dlmalloc',
                  'libnativehelper/header_only_include', 'libnativehelper/include_jni',
                  'libnativehelper/include_platform', 'libnativehelper/include_platform_header_only',
                  'cpu_features/include', 'tinyxml2', 'unwinding/libunwindstack/include',
                  'modules-utils/build/include', 'lz4/lib', 'lzma/C', 'perfetto-heap', 'boringssl/src/include')]
    includes += [OUT, PATCHES, WORK / 'vixl/src', art / 'disassembler', ROOT / 'experiments/native-linux']
    flags = ['--target=aarch64-linux-android10000', '-fPIC', '-ftrivial-auto-var-init=zero', '-ffixed-x18', '-mno-outline-atomics',
             '-O2', '-DNDEBUG', '-DART_TARGET', '-DART_TARGET_ANDROID', '-DART_DEFAULT_GC_TYPE_IS_MS',
             '-DART_USE_RESTRICTED_MODE=1', '-DART_PAGE_SIZE_AGNOSTIC=1', '-DBUILDING_LIBART',
             '-DART_NCE', '-DART_ENABLE_CODEGEN_arm64', '-DVIXL_INCLUDE_TARGET_A64',
             '-DVIXL_GENERATE_SIMULATOR_INSTRUCTIONS_VALUE=0', '-DVIXL_CODE_BUFFER_MALLOC', '-DUSE_D8_DESUGAR=1', '-DFMT_HEADER_ONLY', '-DART_FRAME_SIZE_LIMIT=1744',
             '-DART_BASE_ADDRESS=0x70000000', '-DART_BASE_ADDRESS_MIN_DELTA=(-0x1000000)',
             '-DART_BASE_ADDRESS_MAX_DELTA=0x1000000', '-include', str(platform_config),
             *[f'-DART_STACK_OVERFLOW_GAP_{arch}=8192' for arch in ('arm', 'arm64', 'x86', 'x86_64', 'riscv64')],
             *[value for path in includes for value in ('-I', str(path))]]
    cpp_flags = ['-std=gnu++20', '-fno-rtti', '-fno-exceptions', '-Wno-invalid-offsetof']
    names = re.findall(r'name: "([^"]+)"', (art / 'build/flags/art-flags.aconfig').read_text())
    (OUT / 'com_android_art_flags.h').write_text('#pragma once\nnamespace com::android::art::flags {\n' +
        ''.join(f'inline constexpr bool {name}() {{ return false; }}\n' for name in names) + '}\n')
    subprocess.run([TOOL / 'bin/clang++', *flags, *cpp_flags, '-S',
                    art / 'tools/cpp-define-generator/asm_defines.cc', '-o', OUT / 'asm_defines.s'],
                   check=True, capture_output=True, timeout=120)
    (OUT / 'asm_defines.h').write_bytes(subprocess.check_output(['python3',
        art / 'tools/cpp-define-generator/make_header.py', OUT / 'asm_defines.s']))
    for directory, module in [('runtime', 'art_operator_srcs'),
                               ('libartbase', 'art_libartbase_operator_srcs'),
                               ('compiler', 'art_compiler_operator_srcs')]:
        (OUT / (directory + '-operators.cc')).write_bytes(subprocess.check_output(['python3',
            art / 'tools/generate_operator_out.py', art / directory,
            *source_list(art / directory, module)]))
    subprocess.run(['python3', art / 'runtime/interpreter/mterp/gen_mterp.py', OUT / 'mterp_arm64ng.S',
                    *sorted((art / 'runtime/interpreter/mterp/arm64ng').glob('*.S'))], check=True)
    base = source_list(art / 'libartbase', 'libartbase_defaults') + [
        art / 'libartbase/base/globals_unix.cc', art / 'libartbase/base/mem_map_unix.cc',
        OUT / 'libartbase-operators.cc', WORK / 'tinyxml2/tinyxml2.cpp', PATCHES / 'reference_heap.c']
    runtime = source_list(art / 'runtime', 'libart-runtime_common_defaults')
    bp = (art / 'runtime/Android.bp').read_text().split('name: "libart-runtime_common_defaults"', 1)[1]
    arch = bp[bp.index('arm64: {'):]
    runtime += [art / 'runtime' / name for name in re.findall(r'"([^"\n]+)"',
                  re.search(r'\bsrcs:\s*\[(.*?)\]', arch, re.S)[1]) if not name.startswith(':')]
    runtime += [art / 'runtime' / name for name in ('monitor_android.cc', 'runtime_android.cc', 'thread_android.cc')]
    runtime += source_list(art / 'libelffile', 'libelffile-defaults')
    runtime += [art / 'odrefresh/odr_statslog_host.cc', OUT / 'runtime-operators.cc',
                OUT / 'mterp_arm64ng.S', OUT / 'compiler-operators.cc', PATCHES / 'jit_control.cc']
    runtime += [WORK / 'cpu_features/src' / (name + '.c') for name in
                ('filesystem', 'stack_line_reader', 'string_view', 'hwcaps', 'impl_aarch64_linux_or_android')]
    runtime += source_list(art / 'compiler', 'libart-compiler-defaults')
    runtime += [art / 'compiler/optimizing' / name for name in
                ('instruction_simplifier_shared.cc', 'nodes_shared.cc')]
    compiler_bp = (art / 'compiler/Android.bp').read_text().split('name: "libart-compiler-defaults"', 1)[1]
    arm64 = compiler_bp[compiler_bp.index('arm64: {'):]
    runtime += [art / 'compiler' / name for name in re.findall(r'"([^"\n]+)"',
                  re.search(r'\bsrcs:\s*\[(.*?)\]', arm64, re.S)[1])]
    runtime += sorted((WORK / 'vixl/src').glob('*.cc')) + sorted((WORK / 'vixl/src/aarch64').glob('*.cc'))
    compiler = []
    if include_compiler:
        operators = OUT / 'dex2oat-operators.cc'
        operators.write_bytes(subprocess.check_output(['python3', art / 'tools/generate_operator_out.py',
            art / 'dex2oat', art / 'dex2oat/linker/image_writer.h']))
        compiler = source_list(art / 'dex2oat', 'libart-dex2oat-defaults') + [art / 'dex2oat' / name for name in
            ('dex2oat.cc', 'dex2oat_options.cc', 'linker/arm64/relative_patcher_arm64.cc',
             'linker/arm/relative_patcher_arm_base.cc')] + [operators]
    objects = OUT / 'objects'
    objects.mkdir(exist_ok=True)

    @functools.cache
    def dependency_hash(path):
        return sha(Path(path))

    def compile(source):
        obj = objects / (str(source.relative_to(ROOT)).replace('/', '_') + '.o')
        dependencies = obj.with_suffix('.d')
        cached = obj.with_suffix('.json')
        cpp = source.suffix in ('.cc', '.cpp')
        extra = cpp_flags if cpp else ['-D__ASSEMBLY__'] if source.suffix == '.S' else []
        if 'cpu_features' in str(source):
            extra += ['-DSTACK_LINE_READER_BUFFER_SIZE=1024', '-DHAVE_DLFCN_H', '-DHAVE_STRONG_GETAUXVAL']
        if source == art / 'dex2oat/dex2oat.cc':
            extra = [*extra, '-Dmain=nce_art_dex2oat_main']
        command = list(map(str, [TOOL / 'bin' / ('clang++' if cpp else 'clang'), *flags, *extra,
                                '-DART_STATIC_LIBARTBASE', '-MD', '-MF', dependencies,
                                '-c', source, '-o', obj]))
        # 源码恢复会重置文件时间；按编译器记录的实际依赖内容复用对象文件。
        if obj.exists() and cached.exists():
            previous = json.loads(cached.read_text())
            if (previous['command'] == command and previous['object'] == sha(obj) and
                all(Path(path).is_file() and dependency_hash(path) == checksum
                    for path, checksum in previous['dependencies'].items())):
                return obj
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        if result.returncode:
            raise RuntimeError(f'{source}:\n{result.stderr}')
        paths = shlex.split(dependencies.read_text().replace('\\\n', ' ').split(':', 1)[1])
        cached.write_text(json.dumps(dict(command=command, object=sha(obj),
            dependencies={path: dependency_hash(path) for path in [command[0], *paths]})))
        return obj

    with concurrent.futures.ThreadPoolExecutor(6) as pool:
        units = base + runtime + compiler + ([PATCHES / 'dex2oat-main.cc'] if include_compiler else [])
        compiled = dict(zip(units, pool.map(compile, units)))
    libraries = {}
    for directory in ('lib64', 'com.android.art/lib64', 'com.android.runtime/lib64/bionic',
                      'com.android.i18n/lib64', 'com.android.os.statsd/lib64'):
        for path in (WORK / 'android36' / directory).glob('*.so'):
            if path.is_file(): libraries[path.name] = path
    libraries['libc.so'] = libc
    common = ['libz.so', 'liblog.so', 'libartpalette.so', 'libbase.so', 'libc++.so',
              'libc.so', 'libm.so', 'libdl.so', 'libziparchive.so']

    def link(name, units, dependencies):
        binary = OUT / name
        subprocess.run([TOOL / 'bin/aarch64-linux-android35-clang++', '-nostdlib++', '-shared',
            f'-Wl,-soname,{name},-z,defs,-z,max-page-size=16384,-z,common-page-size=16384',
            *[compiled[path] for path in units], *[libraries[lib] for lib in dependencies],
            '-o', binary], check=True, timeout=120)
        libraries[name] = binary

    link('libartbase.so', base, common)
    runtime_libraries = common + ['libdl_android.so', 'libartbase.so', 'libdexfile.so',
         'libprofile.so', 'libnativebridge.so', 'libnativeloader.so', 'libsigchain.so',
         'libunwindstack.so', 'liblz4.so', 'liblzma.so', 'heapprofd_client_api.so']
    if include_compiler:
        libraries['libcrypto.so'] = crypto['libcrypto.so']
        runtime_libraries.append('libcrypto.so')
    link('libart.so', runtime + compiler, runtime_libraries)
    if include_compiler:
        # JNI 依赖 libart.so；编译器入口放入同一 DSO，防止创建两份 runtime 状态。
        subprocess.run([TOOL / 'bin/aarch64-linux-android35-clang++', '-nostdlib++', '-pie',
            '-Wl,--allow-shlib-undefined,-z,defs,-z,max-page-size=16384,-z,common-page-size=16384',
            compiled[PATCHES / 'dex2oat-main.cc'], libraries['libart.so'],
            *[libraries[name] for name in runtime_libraries], '-o', OUT / 'nce-dex2oat'],
            check=True, timeout=120)
    files = {name: sha(OUT / name) for name in required}
    manifest.write_text(json.dumps(dict(build_inputs=fingerprint, inputs=inputs, files=files,
        compiler_dependencies=compiler_dependencies,
        art_flags={name: False for name in names}, execution='restricted-arm64-jit',
        compressed_references='offset-in-high-4GiB-arena'), indent=2) + '\n')
    return {name: OUT / name for name in files}


if __name__ == '__main__':
    print(build())
