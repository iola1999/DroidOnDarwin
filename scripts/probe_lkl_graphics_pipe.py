#!/usr/bin/env python3
"""复用成功的 Direct core，验证图形字符设备与原 SDK qemu pipe 接口。"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys

from build_android_linker import NDK, TOOL
from probe_lkl_translation import build_graphics
from process_observer import OBSERVATION_MAX_BYTES, observe

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
SOURCE = EXP / 'userspace/graphics-pipe-test.c'
MODES = ('no-listener', 'backlog', 'protocol', 'original')
RUNTIME_MANIFEST_SHA = '4b1a45b0ee51725c68f1c29765958d0b60b45a5f2325bedfc61764cd49232249'
DESKTOP_MANIFEST_SHA = '515c9fe37032c16e01048a9a63f83708b314558d5322de2eb41773db09179772'
READELF = Path('/opt/homebrew/opt/llvm/bin/llvm-readelf')
QEMU_EXPORTS = sorted('qemu_pipe_' + name for name in
                      ('open_ns', 'open', 'close', 'read', 'write', 'read_fully', 'write_fully', 'try_again', 'print_error'))


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def remember(inputs, additions):
    for name, value in additions.items():
        name = str(name)
        if name in inputs and inputs[name] != value:
            raise ValueError('同一路径的已保存摘要发生变化：' + name)
        inputs[name] = value


def dependencies(path):
    target, separator, names = path.read_text().replace(chr(92) + '\n', ' ').partition(':')
    if not separator or not target.strip():
        raise ValueError('编译依赖文件格式不同：' + str(path))
    return [Path(name).resolve(strict=True) for name in shlex.split(names)]


def _elf_contract(path):
    symbols = subprocess.check_output([READELF, '--dyn-syms', path], text=True, timeout=30)
    exported = sorted(re.findall(r'^\s*\d+:\s+[0-9a-f]+\s+\d+\s+FUNC\s+GLOBAL\s+DEFAULT\s+(?!UND\b)\S+\s+(\S+)\s*$', symbols, re.M))
    dynamic = subprocess.check_output([READELF, '-d', path], text=True, timeout=30)
    soname = re.findall(r'\(SONAME\).*\[([^\]]+)\]', dynamic)
    needed = sorted(re.findall(r'\(NEEDED\).*\[([^\]]+)\]', dynamic))
    headers = subprocess.check_output([READELF, '-l', path], text=True, timeout=30)
    alignments = [int(value, 16) for value in re.findall(r'^\s*LOAD\s+.*\s+(0x[0-9a-f]+)\s*$', headers, re.M)]
    if len(soname) != 1 or not alignments:
        raise ValueError('qemu pipe ELF 缺少 SONAME 或 PT_LOAD')
    return {'exports': exported, 'soname': soname[0], 'needed': needed, 'load_alignments': alignments}


def elf(path):
    data = path.read_bytes()
    if data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', data, 16) != (3, 183):
        raise ValueError('需要 ARM64 ET_DYN：' + str(path))
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    loads, interpreter = [], None
    for index in range(count):
        kind, _flags, position, _address, _physical, length, _memory, alignment = struct.unpack_from('<IIQQQQQQ', data, offset + size * index)
        if kind == 1:
            loads.append(alignment)
        if kind == 3:
            interpreter = data[position:position + length].rstrip(b'\0').decode()
    if not loads or any(value != 16384 for value in loads):
        raise ValueError('guest PT_LOAD 需要16 KiB对齐：' + str(path))
    return {'path': str(path), 'sha256': sha(path), 'bytes': len(data), 'load_alignments': loads, 'interpreter': interpreter}


def sdk_inputs(main):
    android = main / 'work/native-linux-nce/android'
    runtime, desktop = android / 'android36', android / 'android36-desktop'
    manifests = [(android / 'android36-runtime.json', RUNTIME_MANIFEST_SHA),
                 (desktop / 'manifest.json', DESKTOP_MANIFEST_SHA)]
    for path, expected in manifests:
        if sha(path) != expected:
            raise ValueError('原 SDK 来源清单不同：' + str(path))
    runtime_files, desktop_files = (json.loads(path.read_text())['files'] for path, _expected in manifests)
    records = {}
    names = {'sdk-linker64': 'com.android.runtime/bin/linker64',
             'sdk-libc.so': 'com.android.runtime/lib64/bionic/libc.so',
             'sdk-libdl.so': 'com.android.runtime/lib64/bionic/libdl.so',
             'sdk-libm.so': 'com.android.runtime/lib64/bionic/libm.so',
             'sdk-liblog.so': 'lib64/liblog.so'}
    for name, key in names.items():
        path, expected = runtime / key, runtime_files[key]
        if sha(path) != expected:
            raise ValueError('原 SDK runtime 摘要不同：' + key)
        records[name] = {'path': str(path), 'sha256': expected}
    for name, key in {'sdk-qemupipe.so': 'vendor/lib64/libqemupipe.ranchu.so',
                      'sdk-libc++.so': 'vendor/lib64/libc++.so'}.items():
        path = desktop / key
        expected = desktop_files[key]
        if sha(path) != expected:
            raise ValueError('原 SDK vendor 摘要不同：' + key)
        records[name] = {'path': str(path), 'sha256': expected}
    original = Path(records['sdk-qemupipe.so']['path'])
    contract = _elf_contract(original)
    if (contract['exports'] != QEMU_EXPORTS or contract['soname'] != 'libqemupipe.ranchu.so' or
            contract['needed'] != ['libc++.so', 'libc.so', 'libdl.so', 'liblog.so', 'libm.so'] or
            any(value != 16384 for value in contract['load_alignments'])):
        raise ValueError('原 qemu pipe 的九导出或 ELF 契约不同')
    return records, {str(path): expected for path, expected in manifests}, contract


def core_inputs(baseline):
    report_path = baseline / 'summary.json'
    report = json.loads(report_path.read_text())
    config = report['configuration']
    if (config['execution'] != 'direct' or config['mapping'] != 'pte' or config['cpus'] != 1 or
            config['workload'] != 'core' or config['guest_layout'] != 'sparse' or not config['data_tbi'] or
            config['code_memory'] != 'dual' or config['direct']['memory'] != 'flat' or
            config['guest_page_bytes'] != 16384 or config.get('bpf_jit') is not True or
            {row['entry'] for row in report['runs']} != {'cli', 'gcd'} or
            not all(row['returncode'] == 0 and row['validated'] for row in report['runs'])):
        raise ValueError('需要通过 CLI/GCD 的单 CPU、16 KiB Direct/PTE/flat/sparse/TBI/dual core')
    guest_command = next(command for command in report['commands'] if '-c' in command and
                         command[command.index('-c') + 1].endswith('/translation/linux-guest.c'))
    source_root = Path(guest_command[guest_command.index('-c') + 1]).parents[3]
    inputs = {}
    for name, value in report['sha256'].items():
        remember(inputs, {str((source_root / name).resolve(strict=True)): value})
    for name, expected in inputs.items():
        if sha(name) != expected:
            raise ValueError('core 实际输入或产物发生变化：' + name)
    images = [Path(name) for name in inputs if name.endswith('/lkl-image.bin')]
    if len(images) != 1:
        raise ValueError('core 缺少唯一内核镜像')
    kernel = images[0]
    kernel_report = kernel.parent / 'build.json'
    if str(kernel_report) not in inputs or sha(kernel_report) != inputs[str(kernel_report)]:
        raise ValueError('core 未保存实际内层 kernel build 报告')
    nested = json.loads(kernel_report.read_text())
    if nested['cpus'] != 1 or nested['guest_page_bytes'] != 16384 or nested.get('bpf_jit') is not True:
        raise ValueError('内层 kernel 构建配置不同')
    kernel_inputs = {}
    for name, value in nested['sha256'].items():
        remember(kernel_inputs, {str((source_root / name).resolve(strict=True)): value})
    for name, expected in kernel_inputs.items():
        if sha(name) != expected:
            raise ValueError('内层 kernel 实际来源发生变化：' + name)
    for name in ('experiments/native-linux/graphics/lkl-graphics-pipe.c',
                 'experiments/native-linux/graphics/graphics-socket-address.h'):
        original = str((source_root / name).resolve(strict=True))
        if kernel_inputs.get(original) != sha(ROOT / name):
            raise ValueError('内层 kernel 未包含当前字符设备来源：' + name)
    hosts = {}
    for entry in ('cli', 'gcd'):
        commands = [command.copy() for command in report['commands'] if '-o' in command and
                    Path(command[command.index('-o') + 1]).resolve() == (baseline / entry).resolve()]
        if len(commands) != 1 or inputs.get(str((baseline / entry).resolve())) != sha(baseline / entry):
            raise ValueError('core 宿主编译输出或摘要不同：' + entry)
        hosts[entry] = commands[0]
    remember(inputs, kernel_inputs)
    return report, report_path, kernel, inputs, hosts


def generated_runner():
    text = (EXP / 'translation/linux-runner.c').read_text()
    marker = 'static void stock_bionic_setup(void)'
    setup = '''static void graphics_pipe_sdk_setup(void)
{
    const char *directories[] = {"/apex", "/apex/com.android.runtime", "/apex/com.android.runtime/bin",
                                "/apex/com.android.runtime/lib64", "/apex/com.android.runtime/lib64/bionic",
                                "/system", "/system/bin", "/system/lib64", "/vendor", "/vendor/lib64", "/linkerconfig"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        directory(directories[i]);
    const char *devices[] = {"/dev/null", "/dev/urandom"};
    const unsigned numbers[] = {259, 265};
    for (unsigned i = 0; i < sizeof(devices) / sizeof(*devices); ++i) {
        long create[6] = {-100, (long)devices[i], 0020666, numbers[i]};
        REQUIRE(call(33, create) == 0);
    }
    const char *files[][2] = {
        {"/system/bin/linker64", "sdk-linker64"},
        {"/apex/com.android.runtime/lib64/bionic/libc.so", "sdk-libc.so"},
        {"/apex/com.android.runtime/lib64/bionic/libdl.so", "sdk-libdl.so"},
        {"/apex/com.android.runtime/lib64/bionic/libm.so", "sdk-libm.so"},
        {"/system/lib64/liblog.so", "sdk-liblog.so"},
        {"/vendor/lib64/libc++.so", "sdk-libc++.so"},
        {"/vendor/lib64/libqemupipe.ranchu.so", "sdk-qemupipe.so"},
        {"/system/bin/graphics-pipe-test", "guest.elf"},
        {"/linkerconfig/ld.config.txt", "sdk-ld.config.txt"}
    };
    for (unsigned i = 0; i < sizeof(files) / sizeof(*files); ++i)
        put_file(files[i][0], files[i][1]);
}

'''
    if text.count(marker) != 1:
        raise ValueError('runner SDK 准备插入位置变化')
    text = text.replace(marker, setup + marker)
    before = '''        seed_entropy();
        translation_memory_phase("guest_execution");
        failed = run_program("/probe-guest", 0);'''
    after = '''        seed_entropy();
        const char *mode = getenv("NCE_GRAPHICS_PIPE_MODE");
        REQUIRE(mode && (!strcmp(mode, "no-listener") || !strcmp(mode, "backlog") ||
                         !strcmp(mode, "protocol") || !strcmp(mode, "original")));
        put_text("/graphics-pipe-mode", mode);
        int original = !strcmp(mode, "original");
        int graphics = original || !strcmp(mode, "protocol");
        if (original)
            graphics_pipe_sdk_setup();
        if (graphics)
            REQUIRE(nce_graphics_start(call) == 0);
        translation_memory_phase("guest_execution");
        failed = run_program(original ? "/system/bin/graphics-pipe-test" : "/probe-guest", 0);
        if (graphics)
            nce_graphics_stop();'''
    if text.count(before) != 1:
        raise ValueError('runner core 执行位置变化')
    return text.replace(before, after)


def validate(rows, mode):
    events = [row for row in rows if row.get('test', '').startswith('graphics_pipe_')]
    names = ['graphics_pipe_node']
    if mode == 'no-listener':
        names += ['graphics_pipe_no_listener'] * 2
    elif mode == 'backlog':
        names += ['graphics_pipe_backlog'] * 2 + ['graphics_pipe_backlog_recovered']
    else:
        if mode == 'original':
            names.insert(0, 'graphics_pipe_original_library')
        names += ['graphics_pipe_scalar', 'graphics_pipe_refcount', 'graphics_pipe_zero_length',
                  'graphics_pipe_vectors_poll', 'graphics_pipe_process_lifetime', 'graphics_pipe_process_lifetime']
        if mode == 'original':
            names.append('graphics_pipe_original_eof_retry')
        names.append('graphics_pipe_early_close')
    names.append('graphics_pipe_guest_cleanup')
    if [row['test'] for row in events] != names or any(row.get('passed') is not True for row in events):
        return False
    node = next(row for row in events if row['test'] == 'graphics_pipe_node')
    if node['major'] != 10 or type(node['minor']) is not int or not 0 <= node['minor'] <= 255 or node['mode'] != 384:
        return False
    if events[-1] != {'test': 'graphics_pipe_guest_cleanup', 'passed': True, 'mode': mode}:
        return False
    if mode in ('no-listener', 'backlog'):
        errors = [row for row in events if row['test'] == 'graphics_pipe_' + mode.replace('-', '_')]
        if ([row['nonblocking'] for row in errors] != [False, True] or
                any(row['errno'] != (111 if mode == 'no-listener' else 11) or
                    type(row['elapsed_ns']) is not int or row['elapsed_ns'] < 0 for row in errors)):
            return False
    else:
        scalar = next(row for row in events if row['test'] == 'graphics_pipe_scalar')
        reference = next(row for row in events if row['test'] == 'graphics_pipe_refcount')
        vectors = next(row for row in events if row['test'] == 'graphics_pipe_vectors_poll')
        lifetime = [row for row in events if row['test'] == 'graphics_pipe_process_lifetime']
        closed = next(row for row in events if row['test'] == 'graphics_pipe_early_close')
        if (scalar['renderer_version'] <= 0 or vectors['renderer_version'] != scalar['renderer_version'] or
                reference['handle'] <= 0 or reference['handle_bytes'] != 4 or vectors['empty_errno'] != 11 or
                not vectors['initial_poll'] & 4 or vectors['initial_poll'] & (1 | 8 | 16) or not vectors['reply_poll'] & 1 or
                not 0 < lifetime[0]['uid'] < lifetime[1]['uid'] or
                any(row['uid_bytes'] != 8 or row['unix_baseline'] < 1 or row['unix_held'] != row['unix_baseline'] + 2 or
                    row['unix_released'] != row['unix_baseline'] for row in lifetime) or
                closed['confirmation'] != 99 or closed['read_errno'] != 5 or closed['readv_errno'] != 5 or
                closed['write_errno'] != 32 or not closed['poll'] & 16):
            return False
    contexts = [row for row in rows if row.get('test') == 'translated_context_cleanup']
    channels = [row for row in rows if row.get('test') == 'native_graphics_channels_released']
    expected_channels = ([{'test': 'native_graphics_channels_released', 'passed': True, 'opened': 6, 'closed': 6}]
                         if mode in ('protocol', 'original') else [])
    return (len(contexts) == 1 and contexts[0].get('allocated', 0) > 0 and
            contexts[0]['allocated'] == contexts[0].get('freed') and channels == expected_channels and
            [row for row in rows if 'result' in row] == [{'result': 'passed'}] and
            [row for row in rows if row.get('test') == 'translated_bpf_code_pool_cleanup'] ==
            [{'test': 'translated_bpf_code_pool_cleanup', 'passed': True}])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, help='当前成功 core 的目录')
    parser.add_argument('--output', type=Path, required=True, help='work 下的报告目录')
    parser.add_argument('--build-output', type=Path, required=True, help='work/native-linux-nce 下的独立生成目录')
    parser.add_argument('--graphics-prefix', type=Path)
    parser.add_argument('--graphics-angle', type=Path)
    parser.add_argument('--prepare-only', action='store_true', help='只编译和生成，不运行 guest')
    args = parser.parse_args()
    if not args.prepare_only and not args.baseline:
        parser.error('运行需要明确指定成功 core')
    common = Path(subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    main_root = common.resolve().parent
    out, build = args.output.resolve(), args.build_output.resolve()
    if not any(out.is_relative_to(base / 'work') for base in (ROOT, main_root)) or not any(
            build.is_relative_to(base / 'work/native-linux-nce') for base in (ROOT, main_root)):
        parser.error('报告和生成目录需要位于项目指定 work 范围')
    if out == build or out.is_relative_to(build) or build.is_relative_to(out):
        parser.error('报告目录与生成目录需要独立')
    out.mkdir(parents=True, exist_ok=False)
    build.mkdir(parents=True, exist_ok=False)
    result_path = out / 'result.json'
    state = {'schema': 1, 'status': 'preparing', 'scope': 'LKL字符设备与实际gfxstream；原qemu DSO按原字节加载，完整Android与iPhone待验证',
             'platform': platform.platform(), 'command': [sys.executable, *sys.argv], 'source_inputs': {},
             'commands': [], 'runs': [], 'limitations': ['设备节点由probe按实际sysfs动态minor创建，权限0600；原SELinux和ueventd由raw init另验',
                 '无listener/backlog使用受控Linux listener；协议与原DSO模式使用真实gfxstream/ANGLE',
                 'refcount检查原接口开管、单次四字节写与FD释放；ColorBuffer异步销毁、像素与持续性能另验']}

    def save():
        result_path.write_text(json.dumps(state, ensure_ascii=False, indent=2) + '\n')

    def run(command, label):
        command = list(map(str, command))
        log = out / (label + '.log')
        row = {'command': command, 'log': str(log), 'timeout_seconds': 180}
        state['commands'].append(row)
        save()
        with log.open('xb') as stream:
            completed = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, timeout=180)
        row.update(returncode=completed.returncode, log_sha256=sha(log))
        save()
        if completed.returncode:
            raise ValueError('编译或生成命令失败：' + str(log))

    def compile_guest(flags, trailing, label):
        before, compiled = build / (label + '-before.d'), build / (label + '.d')
        run([*flags, '-M', '-MF', before, SOURCE], 'dependencies-' + label)
        before_inputs = {str(path): sha(path) for path in dependencies(before)}
        remember(inputs, before_inputs)
        run([*flags, '-MD', '-MF', compiled, SOURCE, *trailing], 'compile-' + label)
        compiled_inputs = {str(path): sha(path) for path in dependencies(compiled)}
        unchanged = before_inputs == compiled_inputs
        state.setdefault('guest_dependencies', {})[label] = {
            'before_sha256': sha(before), 'compiled_sha256': sha(compiled),
            'input_count': len(before_inputs), 'unchanged': unchanged}
        save()
        if not unchanged:
            raise ValueError('guest 编译前后依赖集合或摘要发生变化：' + label)
        remember(inputs, compiled_inputs)

    save()
    try:
        paths = [SOURCE, Path(__file__).resolve(), ROOT / 'scripts/build_android_linker.py',
                 READELF, ROOT / 'scripts/probe_lkl_translation.py',
                 ROOT / 'scripts/process_observer.py', EXP / 'translation/linux-runner.c']
        inputs = state['source_inputs']
        remember(inputs, {str(path): sha(path) for path in paths})
        sdk, manifests, contract = sdk_inputs(main_root)
        state['sdk'] = {'files': sdk, 'manifests': manifests, 'qemupipe_contract': contract}
        remember(inputs, manifests)
        remember(inputs, {record['path']: record['sha256'] for record in sdk.values()})
        if 'Pkg.Revision = 29.0.14206865' not in (NDK / 'source.properties').read_text():
            raise ValueError('需要 Android NDK 29.0.14206865')
        remember(inputs, {str(NDK / 'source.properties'): sha(NDK / 'source.properties')})
        run(['bash', 'scripts/format.sh', '--check', SOURCE], 'format')
        llvm = Path('/opt/homebrew/opt/llvm/bin')
        musl = ROOT / 'work/native-linux-nce/musl-build/install'
        resource = Path(subprocess.check_output([llvm / 'clang', '-print-resource-dir'], text=True, timeout=10).strip()) / 'include'
        static_object = build / 'guest.o'
        static_flags = [llvm / 'clang', '--target=aarch64-linux-musl', '-mno-outline-atomics', '-fPIE', '-O2', '-g',
                        '-Wall', '-Wextra', '-Werror', '-nostdinc', '-isystem', musl / 'include', '-isystem', resource]
        link_static = [shutil.which('ld.lld'), '-pie', '--gc-sections', '--no-dynamic-linker', '-z', 'max-page-size=16384',
                       '-z', 'common-page-size=16384', musl / 'lib/rcrt1.o', musl / 'lib/crti.o', static_object,
                       musl / 'lib/libc.a', ROOT / 'work/native-linux-nce/compiler-rt-build/libbuiltins.a', musl / 'lib/crtn.o',
                       '-o', build / 'static.elf']
        remember(inputs, {str(path): sha(path) for path in (llvm / 'clang', Path(link_static[0]), musl / 'lib/rcrt1.o',
                          musl / 'lib/crti.o', musl / 'lib/libc.a', ROOT / 'work/native-linux-nce/compiler-rt-build/libbuiltins.a', musl / 'lib/crtn.o')})
        compile_guest(static_flags, ['-c', '-o', static_object], 'static')
        run(link_static, 'link-static')
        headers = main_root / 'work/native-linux-nce/graphics-source/gfxstream-guest/qemupipe'
        original_libraries = [Path(sdk[name]['path']) for name in
                              ('sdk-qemupipe.so', 'sdk-libc++.so', 'sdk-liblog.so', 'sdk-libc.so', 'sdk-libdl.so', 'sdk-libm.so')]
        remember(inputs, {str(TOOL / 'bin/clang'): sha(TOOL / 'bin/clang')})
        original_flags = [TOOL / 'bin/clang', '--target=aarch64-linux-android35', '-O2', '-g', '-fPIE', '-ffixed-x18',
                          '-mno-outline-atomics', '-Wall', '-Wextra', '-Werror', '-DNCE_GRAPHICS_PIPE_ORIGINAL',
                          '-I', headers / 'include', '-I', headers / 'include-types']
        compile_guest(original_flags, [*original_libraries, '-Wl,-z,defs,-z,max-page-size=16384,-z,common-page-size=16384',
                                      '-o', build / 'original.elf'], 'original')
        state['guest_elf'] = {'static': elf(build / 'static.elf'), 'original': elf(build / 'original.elf')}
        if state['guest_elf']['static']['interpreter'] is not None or state['guest_elf']['original']['interpreter'] != '/system/bin/linker64':
            raise ValueError('static/original guest interpreter 不同')
        runner = build / 'linux-runner.c'
        runner.write_text(generated_runner())
        state['generated'] = {'runner': str(runner), 'sha256': sha(runner)}
        if args.baseline:
            baseline = args.baseline.resolve(strict=True)
            core, core_report, kernel, base_inputs, host_commands = core_inputs(baseline)
            remember(inputs, base_inputs)
            remember(inputs, {str(core_report): sha(core_report)})
            state['core'] = {'directory': str(baseline), 'report_sha256': sha(core_report), 'configuration': core['configuration'],
                             'kernel': str(kernel), 'nested_kernel_report_sha256': sha(kernel.parent / 'build.json')}
            prefix = (args.graphics_prefix or main_root / 'work/gfxstream-build-macos-ios-api/prefix').resolve(strict=True)
            angle = (args.graphics_angle or main_root / 'work/angle-macos/prefix/lib').resolve(strict=True)
            graphics_command, graphics_link, graphics_files = build_graphics(build, prefix=prefix, angle=angle)
            state['commands'].append({'command': graphics_command, 'result': 'passed'})
            remember(inputs, {str(path): sha(path) for path in graphics_files})
            extra = [EXP / name for name in ('input.c', 'session.c', 'session-profile.c')]
            remember(inputs, {str(path): sha(path) for path in extra})
            hosts = {}
            for entry, command in host_commands.items():
                original_runner = str(EXP / 'translation/linux-runner.c')
                if command.count(original_runner) != 1:
                    raise ValueError('core runner 编译来源不同')
                command[command.index(original_runner)] = str(runner)
                command[command.index('-o') + 1] = str(build / entry)
                command += ['-iquote', str(EXP / 'translation'), *map(str, extra),
                            *[str(value) for value in graphics_link if str(value) != '-lc++' or '-lc++' not in command]]
                run(command, 'compile-' + entry)
                hosts[entry] = build / entry
            state['host_outputs'] = {entry: {'path': str(path), 'sha256': sha(path)} for entry, path in hosts.items()}
        elif not args.prepare_only:
            raise ValueError('运行缺少 core')
        if args.prepare_only:
            state['status'] = 'prepared'
            state['guest_execution'] = 'pending'
        else:
            config = core['configuration']
            environment = {name: value for name, value in os.environ.items() if not name.startswith(('NCE_', 'ANGLE_'))}
            environment.update(DYLD_LIBRARY_PATH=str(angle), ANGLE_DEFAULT_PLATFORM='metal',
                NCE_TRANSLATION_GUEST_LAYOUT='sparse', NCE_TRANSLATION_DATA_TBI='1', NCE_TRANSLATION_ONLINE_CPUS='1',
                NCE_TRANSLATION_BATCH=str(config['instruction_batch']), NCE_TRANSLATION_PARALLEL_PROFILE='0',
                NCE_TRANSLATION_UACCESS_STOP=config['uaccess_stop'], NCE_TRANSLATION_PREEMPTION=config['preemption'],
                NCE_TRANSLATION_MEMORY_MS=str(config['memory_interval_ms']), NCE_TRANSLATION_WORKLOAD='core',
                NCE_TRANSLATION_CACHE=config['cache_invalidation'], NCE_TRANSLATION_EXEC_CACHE=config['exec_cache'],
                NCE_TRANSLATION_TLB=config['tlb_invalidation'], NCE_TRANSLATION_LINUX_FASTPATH='0',
                NCE_TRANSLATION_CACHE_MIB=str(config['translation_cache_bytes'] // (1024 * 1024)))
            for name, value in config['direct'].items():
                environment['NCE_DIRECT_' + name.upper()] = str(int(value)) if isinstance(value, bool) else str(value)
            state['environment'] = {name: value for name, value in environment.items() if name.startswith(('NCE_', 'ANGLE_', 'DYLD_'))}
            state['status'] = 'running'
            runtime_names = ('sigreturn.elf', 'dynamic.elf', 'libc.so', 'dependency.so', 'plugin.so', 'dash.elf', 'bionic-test.elf')
            runtime_names += tuple(path.name for path in sorted(baseline.glob('exec-aslr-*.elf')))
            for mode in MODES:
                runtime = build / ('runtime-' + mode)
                runtime.mkdir()
                for name in runtime_names:
                    (runtime / name).symlink_to(baseline / name)
                guest = build / ('original.elf' if mode == 'original' else 'static.elf')
                (runtime / 'guest.elf').symlink_to(guest)
                (runtime / 'late.elf').symlink_to(guest)
                if mode == 'original':
                    for name, record in sdk.items():
                        (runtime / name).symlink_to(record['path'])
                    (runtime / 'sdk-ld.config.txt').write_text('dir.pipe = /system/bin\n\n[pipe]\nnamespace.default.isolated = false\n'
                        'namespace.default.search.paths = /vendor/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64\n')
                for entry, host in hosts.items():
                    if (sha(host) != state['host_outputs'][entry]['sha256'] or sha(runner) != state['generated']['sha256'] or
                            sha(guest) != state['guest_elf']['original' if mode == 'original' else 'static']['sha256'] or
                            not all(sha(path) == expected for path, expected in inputs.items())):
                        raise ValueError('启动前实际输入发生变化')
                    run_environment = {**environment, 'NCE_TRANSLATION_FILES': str(runtime), 'NCE_GRAPHICS_PIPE_MODE': mode}
                    command = [str(host), str(kernel)]
                    stdout, stderr = out / (entry + '-' + mode + '.jsonl'), out / (entry + '-' + mode + '.log')
                    row = {'entry': entry, 'mode': mode, 'command': command, 'stdout': str(stdout), 'stderr': str(stderr), 'timeout_seconds': 45}
                    state['runs'].append(row)
                    save()
                    with stdout.open('xb') as output_stream, stderr.open('xb') as error_stream:
                        result = observe(command, run_environment, output_stream, error_stream, 45)
                    with stdout.open('rb') as stream:
                        data = stream.read(OBSERVATION_MAX_BYTES + 1)
                    errors, records = [], []
                    for index, line in enumerate(data.decode(errors='replace').splitlines(), 1):
                        try:
                            records.append(json.loads(line))
                        except json.JSONDecodeError:
                            errors.append(index)
                    with stderr.open('rb') as stream:
                        error_data = stream.read(OBSERVATION_MAX_BYTES + 1)
                    kernel_fault = b'BUG: non-zero pgtables_bytes on freeing mm:' in error_data
                    row.update(host_process=result, results=records, parse_error_lines=errors, kernel_pgtables_fault=kernel_fault,
                               stdout_sha256=sha(stdout), stderr_sha256=sha(stderr))
                    row['validated'] = (result['returncode'] == result['actual_exit'] == 0 and not result['timed_out'] and
                        not result['errors'] and not result['cleanup_signals'] and result['process_group_empty'] and
                        not result['output_limit_exceeded'] and max(len(data), len(error_data)) <= OBSERVATION_MAX_BYTES and not kernel_fault and
                        not errors and validate(records, mode))
                    save()
                    print(json.dumps({'entry': entry, 'mode': mode, 'validated': row['validated'], 'returncode': result['returncode']}, ensure_ascii=False), flush=True)
                    if not row['validated']:
                        raise ValueError('字符设备运行验收失败：' + entry + '/' + mode)
            state['status'] = 'passed'
        state['inputs_unchanged'] = all(sha(path) == expected for path, expected in inputs.items())
        state['generated_unchanged'] = (sha(runner) == state['generated']['sha256'] and
            all(sha(record['path']) == record['sha256'] for record in state['guest_elf'].values()) and
            all(sha(record['path']) == record['sha256'] for record in state.get('host_outputs', {}).values()))
        if not state['inputs_unchanged'] or not state['generated_unchanged']:
            raise ValueError('本轮实际输入发生变化')
        if state['status'] == 'passed':
            cleanup = main_root / ('work/cleanup-' + datetime.now().strftime('%Y-%m-%d'))
            cleanup.mkdir(exist_ok=True)
            cleanup_path = cleanup / (out.name + '.json')
            if cleanup_path.exists():
                raise ValueError('清理记录已存在：' + str(cleanup_path))
            removed, directories = [], []
            for path in sorted(build.rglob('*')):
                if path.is_symlink():
                    removed.append({'path': str(path), 'kind': 'symlink', 'target': os.readlink(path)})
                elif path.is_file():
                    removed.append({'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)})
                elif path.is_dir():
                    directories.append(str(path))
            shutil.rmtree(build)
            directories.append(str(build))
            cleanup_path.write_text(json.dumps({'removed': removed, 'removed_directories': directories,
                'classification': '本轮独立生成源码、执行文件、依赖和只读输入引用；全部进程组已结束',
                'result': str(result_path)}, ensure_ascii=False, indent=2) + '\n')
            state['cleanup'] = {'path': str(cleanup_path), 'sha256': sha(cleanup_path)}
    except BaseException as error:
        state.update(status='failed', error={'type': type(error).__name__, 'message': str(error)})
        raise
    finally:
        save()
    print(json.dumps({'status': state['status'], 'report': str(result_path), 'runs': len(state['runs'])}, ensure_ascii=False))


if __name__ == '__main__':
    main()
