#!/usr/bin/env python3
"""编译内置 Linux 程序，按 ELF 段与 AArch64 映射符号生成执行布局。"""
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
WORK = ROOT / 'work/native-linux-nce'
PAGE = 16384
PROGRAM_SLOT = 2 * 1024 * 1024
INTERPRETER = '/lib/ld-musl-aarch64.so.1'
MUSL_SHA = 'a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4'
RT_SHA = '15277402f6fd63397c0917a5c7171cda82d16d226094b828c1ed0f58f73b9c69'
DASH_SHA = '6a474ac46e8b0b32916c4c60df694c82058d3297d8b385b74508030ca4a8f28a'


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, timeout=300, **kw)


def musl(llvm):
    source = WORK / 'musl-1.2.5'
    archive = WORK / 'musl-1.2.5.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve('https://musl.libc.org/releases/musl-1.2.5.tar.gz', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != MUSL_SHA:
        raise RuntimeError('musl source archive hash mismatch')
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(WORK, filter='data')
    build = WORK / 'musl-build'
    build.mkdir(exist_ok=True)
    target = [llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics']
    with (build / 'build.log').open('w') as log:
        if not (build / 'install/lib/libc.so').exists():
            run([source / 'configure', '--target=aarch64-linux-musl', '--enable-shared',
                 '--prefix=' + str(build / 'install'), '--syslibdir=' + str(build / 'install/lib'),
                 'CC=' + shlex.join(map(str, target)), 'AR=' + str(llvm / 'llvm-ar'),
                 'RANLIB=' + str(llvm / 'llvm-ranlib'), 'CFLAGS=-O2 -fPIC',
                 'LDFLAGS=-fuse-ld=lld -Wl,-z,max-page-size=16384,-z,common-page-size=16384', 'LIBCC='],
                cwd=build, stdout=log, stderr=subprocess.STDOUT)
        run(['gmake', '-j8', 'install'], cwd=build, stdout=log, stderr=subprocess.STDOUT)
    return build / 'install'


def inspect(path, shared=False):
    data = path.read_bytes()
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', data, 16) != (3, 183):
        raise RuntimeError(f'{path}: requires AArch64 ET_DYN')
    entry, phoff, shoff = struct.unpack_from('<QQQ', data, 24)
    phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', data, 54)
    if phentsize != 56 or shentsize != 64:
        raise RuntimeError('Unexpected ELF table format')
    if phoff + phnum * phentsize > len(data) or shoff + shnum * shentsize > len(data):
        raise RuntimeError(f'{path}: truncated ELF tables')
    phdrs = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize) for i in range(phnum)]
    loads = [p for p in phdrs if p[0] == 1]
    if loads != sorted(loads, key=lambda p: p[3]):
        raise RuntimeError(f'{path}: load segments are out of address order')
    interpreters = [p for p in phdrs if p[0] == 3]
    if len(interpreters) > 1:
        raise RuntimeError(f'{path}: multiple ELF interpreters')
    for p in interpreters:
        if p[2] + p[5] > len(data) or data[p[2]:p[2] + p[5]] != INTERPRETER.encode() + b'\0':
            raise RuntimeError(f'{path}: unsupported ELF interpreter')
    segments = []
    for _, flags, offset, va, _, filesz, memsz, align in loads:
        if align > PAGE or (va - offset) % PAGE or filesz > memsz or offset + filesz > len(data):
            raise RuntimeError(f'{path}: unsupported segment layout')
        if va + memsz > PROGRAM_SLOT:
            raise RuntimeError(f'{path}: exceeds the current per-program range')
        if flags & 1 and memsz:
            if flags & 2:
                raise RuntimeError(f'{path}: writable executable segment')
            lo, hi = va & -PAGE, (va + memsz + PAGE - 1) & -PAGE
            if any(p[1] & 2 and (p[3] & -PAGE) < hi and ((p[3] + p[6] + PAGE - 1) & -PAGE) > lo for p in loads):
                raise RuntimeError(f'{path}: executable and writable segments share a host page')
            segments.append((lo, hi - lo))
    if any(a + size > b for (a, size), (b, _) in zip(segments, segments[1:])):
        raise RuntimeError(f'{path}: executable segments share a host page')
    if not segments or not (shared and entry == 0) and not any(p[1] & 1 and p[3] <= entry < p[3] + p[5] for p in loads):
        raise RuntimeError(f'{path}: missing executable entry')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    mappings = {}
    for symtab in sections:
        if symtab[1] != 2:
            continue
        if symtab[9] != 24 or symtab[5] % 24 or symtab[6] >= len(sections) or symtab[4] + symtab[5] > len(data):
            raise RuntimeError(f'{path}: invalid symbol table')
        strings = sections[symtab[6]]
        if strings[4] + strings[5] > len(data):
            raise RuntimeError(f'{path}: truncated symbol strings')
        strings = data[strings[4]:strings[4] + strings[5]]
        for off in range(symtab[4], symtab[4] + symtab[5], symtab[9]):
            name, _, _, sec, addr, _ = struct.unpack_from('<IBBHQQ', data, off)
            end = strings.find(b'\0', name)
            if end < 0:
                raise RuntimeError(f'{path}: invalid symbol name')
            name = strings[name:end].decode()
            if re.fullmatch(r'\$[xd](?:\..*)?', name):
                mappings.setdefault(sec, set()).add((addr, name[1]))
    patches = []
    for index, sec in enumerate(sections):
        if not sec[2] & 4 or not sec[5]:
            continue
        if not any(p[1] & 1 and p[3] <= sec[3] and sec[3] + sec[5] <= p[3] + p[5] and
                   sec[4] - p[2] == sec[3] - p[3] for p in loads):
            raise RuntimeError(f'{path}: executable section is outside its file segment')
        markers = sorted(mappings.get(index, []))
        # LLD's AArch64 PLT carries SHF_AARCH64_PURECODE instead of $x symbols.
        if not markers and sec[2] & 0x20000000:
            markers = [(sec[3], 'x')]
        if not markers or markers[0][0] != sec[3]:
            raise RuntimeError(f'{path}: executable section lacks mapping symbols')
        for i, (begin, kind) in enumerate(markers):
            end = markers[i + 1][0] if i + 1 < len(markers) else sec[3] + sec[5]
            if begin >= end or end > sec[3] + sec[5]:
                raise RuntimeError(f'{path}: invalid instruction mapping span')
            if kind != 'x':
                continue
            if begin % 4 or end % 4:
                raise RuntimeError(f'{path}: unaligned instruction span')
            for address in range(begin, end, 4):
                word, = struct.unpack_from('<I', data, sec[4] + address - sec[3])
                if word & 0xffe0001f == 0xd4000001 or word & 0xffffffe0 in (0xd53bd040, 0xd51bd040):
                    patches.append((address, word))
    return dict(data=data, entry=entry, phoff=phoff, phsize=phnum*phentsize, segments=segments, patches=patches)


def compiler_runtime(llvm, libc):
    source = WORK / 'compiler-rt-20.1.8.src'
    archive = WORK / (source.name + '.tar.xz')
    if not archive.exists():
        urllib.request.urlretrieve('https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/' + archive.name, archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != RT_SHA:
        raise RuntimeError('compiler-rt source archive hash mismatch')
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(WORK, filter='data')
    build = WORK / 'compiler-rt-build'
    build.mkdir(exist_ok=True)
    library = build / 'libbuiltins.a'
    if not library.exists() or not (build / 'muldc3.o').exists():
        builtins = source / 'lib/builtins'
        cmake = (builtins / 'CMakeLists.txt').read_text()
        files = re.search(r'set\(GENERIC_TF_SOURCES\s+(.*?)\)', cmake, re.S)[1].split()
        files += ['clzti2.c', 'ctzti2.c', 'udivti3.c', 'udivmodti4.c', 'divti3.c', 'modti3.c', 'umodti3.c', 'muldc3.c', 'mulsc3.c', 'aarch64/fp_mode.c']
        objects = []
        for name in files:
            obj = build / (Path(name).stem + '.o')
            run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
                 '-O2', '-fPIC', '-fno-builtin', '-fvisibility=hidden', '-isystem', libc / 'include',
                 '-c', builtins / name, '-o', obj])
            objects.append(obj)
        run([llvm / 'llvm-ar', 'rcs', library, *objects])
    return library


def dash(out, llvm, libc, runtime, resource):
    source = WORK / 'dash-0.5.12'
    archive = WORK / 'dash-0.5.12.tar.gz'
    if not archive.exists():
        urllib.request.urlretrieve('https://sources.buildroot.net/dash/dash-0.5.12.tar.gz', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != DASH_SHA:
        raise RuntimeError('dash source archive hash mismatch')
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(WORK, filter='data')
    build = WORK / 'dash-build'
    build.mkdir(exist_ok=True)
    cc = build / 'target-cc'
    flags = [str(llvm / 'clang'), '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-nostdinc', '-isystem', str(libc / 'include'), '-isystem', str(resource), '-fPIE']
    link = ['-nostdlib', '-fuse-ld=lld', '-Wl,-pie,-dynamic-linker,' + INTERPRETER + ',-z,max-page-size=16384,-z,common-page-size=16384',
            str(libc / 'lib/Scrt1.o'), str(libc / 'lib/crti.o')]
    libraries = ['-L' + str(libc / 'lib'), '-lc', str(runtime), str(libc / 'lib/crtn.o')]
    cc.write_text('#!/usr/bin/env python3\nimport os,sys\n'
                  f'flags={json.dumps(flags)}\nargs=sys.argv[1:]\n'
                  f'if not any(a in args for a in ("-c","-S","-E")) and any(a.endswith((".c",".o")) for a in args):\n'
                  f' args={json.dumps(link)}+args+{json.dumps(libraries)}\n'
                  'os.execv(flags[0], flags+args)\n')
    cc.chmod(0o755)
    with (build / 'build.log').open('w') as log:
        if not (build / 'config.h').exists():
            run([source / 'configure', '--host=aarch64-linux-musl', '--build=aarch64-apple-darwin',
                 '--without-libedit', 'CC=' + str(cc), 'CC_FOR_BUILD=cc', 'CFLAGS=-O2 -g'],
                cwd=build, stdout=log, stderr=subprocess.STDOUT)
        # mksignames runs on macOS but must use the Linux signal numbers.
        generator = (source / 'src/mksignames.c').read_text()
        definitions = dict(re.findall(r'^#define\s+((?:SIG\w+|_NSIG))\s+(\w+)\s*$',
                                     (libc / 'include/bits/signal.h').read_text(), re.M))
        names = set(re.findall(r'\bSIG[A-Z0-9]+\b', generator)) | {'NSIG', '_NSIG'}
        header = '#include <signal.h>\n' + ''.join(f'#undef {name}\n' for name in sorted(names))
        header += ''.join(f'#define {name} {value}\n' for name, value in definitions.items() if name in names)
        # musl 1.2.5 reserves 32..34 for its internal thread signals.
        header += '#define NSIG _NSIG\n#define SIGRTMIN 35\n#define SIGRTMAX 64\n'
        generated = build / 'src/mksignames-target.c'
        generated.write_text(generator.replace('#include <signal.h>', header, 1))
        run(['cc', '-Wno-deprecated-non-prototype', generated, '-o', build / 'src/mksignames'], stdout=log, stderr=subprocess.STDOUT)
        # The wrapper is a build input that make's generated rules do not track.
        (build / 'src/dash').unlink(missing_ok=True)
        run(['gmake', '-j8'], cwd=build, stdout=log, stderr=subprocess.STDOUT)
    shutil.copyfile(build / 'src/dash', out / 'dash.elf')
    return out / 'dash.elf'


def build(out, llvm):
    libc = musl(llvm)
    runtime = compiler_runtime(llvm, libc)
    with (WORK / 'musl-build/shared-build.log').open('w') as log:
        run(['gmake', '-j8', '-W', 'obj/ldso/dynlink.lo', 'LIBCC=' + str(runtime), 'install'],
            cwd=WORK / 'musl-build', stdout=log, stderr=subprocess.STDOUT)
    resource = Path(subprocess.check_output([llvm / 'clang', '-print-resource-dir'], text=True).strip()) / 'include'
    run([llvm / 'clang', '--target=aarch64-linux-gnu', '-c', EXP / 'exec-guest.S', '-o', out / 'exec-guest.o'])
    run([shutil.which('ld.lld'), '-pie', '--no-dynamic-linker', '--entry=_start', '-Ttext=0x10000',
         '-z', 'max-page-size=16384', out / 'exec-guest.o', '-o', out / 'exec-guest.elf'])
    programs = [('/nce-exec', out / 'exec-guest.elf')]
    # Force a real ELF text segment beyond the former 64 KiB limit. The table
    # also exercises static-PIE relocations and indirect calls across that text.
    large = out / 'large-text.c'
    large.write_text('\n'.join(
        f'__attribute__((noinline)) static unsigned f{i}(unsigned x) {{ return (x + {i*7919+17}u) ^ (x >> 3); }}'
        for i in range(4096)) + '\nstatic unsigned (*volatile functions[])(unsigned) = {' +
        ','.join(f'f{i}' for i in range(4096)) + '};\n'
        'unsigned exercise_large_text(unsigned x) { for (unsigned i=0;i<4096;++i) x=functions[i](x); return x; }\n')
    run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
         '-fPIE', '-O2', '-c', large, '-o', out / 'large-text.o'])
    for unit in ('mm-tests', 'signal-tests'):
        run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-fPIE', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
             '-c', EXP / f'userspace/{unit}.c', '-o', out / f'{unit}.o'])
    run([llvm / 'clang', '--target=aarch64-linux-musl', '-c', EXP / 'userspace/signal-registers.S',
         '-o', out / 'signal-registers.o'])
    for name in ('init', 'worker', 'mm-bench'):
        run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-fPIE', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
             '-c', EXP / f'userspace/{name}.c', '-o', out / f'{name}.o'])
        run([shutil.which('ld.lld'), '-pie', '--no-dynamic-linker', '-z', 'max-page-size=16384',
             '-z', 'common-page-size=16384', libc / 'lib/rcrt1.o', libc / 'lib/crti.o',
             out / f'{name}.o', *([out / 'large-text.o'] if name == 'worker' else []),
             *([out / 'mm-tests.o', out / 'signal-tests.o', out / 'signal-registers.o'] if name == 'mm-bench' else []),
             libc / 'lib/libc.a', runtime, libc / 'lib/crtn.o', '-o', out / f'{name}.elf'])
        programs.append(('/init-user' if name == 'init' else '/' + name, out / f'{name}.elf'))
    programs.append(('/bin/sh', dash(out, llvm, libc, runtime, resource)))
    shutil.copyfile(libc / 'lib/libc.so', out / 'libc.so')
    programs.append((INTERPRETER, out / 'libc.so'))
    for name in ('dependency', 'plugin', 'dynamic'):
        run([llvm / 'clang', '--target=aarch64-linux-musl', '-ffixed-x18', '-mno-outline-atomics',
             '-fPIC', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', libc / 'include', '-isystem', resource,
             '-c', EXP / f'userspace/{name}.c', '-o', out / f'{name}.o'])
        options = ['-pie', '-dynamic-linker', INTERPRETER, libc / 'lib/Scrt1.o', libc / 'lib/crti.o'] if name == 'dynamic' else ['-shared', '-soname', name + '.so']
        file = out / (name + ('.elf' if name == 'dynamic' else '.so'))
        run([shutil.which('ld.lld'), *options, '-z', 'max-page-size=16384', '-z', 'common-page-size=16384',
             out / f'{name}.o', *([out / 'dependency.so', '-rpath', '/lib'] if name == 'dynamic' else []),
             '-L' + str(libc / 'lib'), '-lc', runtime, libc / 'lib/crtn.o', '-o', file])
        programs.append(('/dynamic' if name == 'dynamic' else '/lib/' + name + '.so', file))
    if len(programs) * PROGRAM_SLOT > 32 * 1024 * 1024:
        raise RuntimeError('Program slots overlap the mmap area')
    header, entries = [], []
    for i, (name, path) in enumerate(programs):
        elf = inspect(path, shared=name.endswith('.so'))
        if name == '/worker' and sum(size for _, size in elf['segments']) <= 65536:
            raise RuntimeError('Large-text fixture did not exceed 64 KiB')
        dis = subprocess.check_output([llvm / 'llvm-objdump', '-d', path], text=True)
        if re.search(r'\b[wx]18\b', dis):
            raise RuntimeError(f'{path}: uses the Darwin-reserved x18 register')
        header.append(f'static const unsigned char image_{i}[] = {{' + ','.join(map(str, elf['data'])) + '};')
        header.append(f'static NceSegment segments_{i}[] = {{' + ','.join(
            f'{{.offset={lo},.size={size}}}' for lo, size in elf['segments']) + '};')
        header.append(f'static const NcePatch patches_{i}[] = {{' + ','.join(
            f'{{{va},{word}}}' for va, word in elf['patches']) + '};')
        entries.append(f'{{.path="{name}",.file=image_{i},.size=sizeof(image_{i}),.offset={i*PROGRAM_SLOT},'
                       f'.entry={elf["entry"]},.phoff={elf["phoff"]},.phsize={elf["phsize"]},'
                       f'.segments=segments_{i},.segment_count={len(elf["segments"])},'
                       f'.patches=patches_{i},.patch_count={len(elf["patches"])}}}')
        print(f'{name}: {len(elf["data"])} bytes, {len(elf["segments"])} executable segments, {len(elf["patches"])} patched instructions', flush=True)
    header += ['NceProgram nce_programs[] = {' + ',\n'.join(entries) + '};',
               f'const size_t nce_program_count = {len(entries)};']
    (out / 'program-images.h').write_text('\n'.join(header) + '\n')
    (out / 'user-scripts.h').write_text('static const char shell_script[] = ' +
                                       json.dumps((EXP / 'userspace/shell-tests.sh').read_text()) + ';\n')
