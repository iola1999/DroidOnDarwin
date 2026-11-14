#!/usr/bin/env python3
"""构建独立的 PTE 翻译实验内核，保留产品内核产物。"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

import lkl_nce_patch
import lkl_ashmem_patch
import lkl_translation_patch
import probe_lkl

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'
EXP = ROOT / 'experiments/native-linux'
OUT = WORK / 'lkl-translation-kernel'


def build(cpus=1, high_res_timers=False, guest_page_bytes=16384, bpf_jit=False):
    if guest_page_bytes not in (4096, 16384) or (guest_page_bytes == 4096 and cpus != 1):
        raise ValueError("4 KiB translation requires one CPU")
    if cpus not in (1, 2):
        raise ValueError('translation CPU count must be 1 or 2')
    out = OUT if cpus == 1 else WORK / 'lkl-translation-smp-kernel'
    if high_res_timers:
        out = out.with_name(out.name + '-highres')
    if guest_page_bytes == 4096:
        out = out.with_name(out.name + '-4k-vmalloc')
    if bpf_jit:
        if cpus != 1:
            raise ValueError('LKL ARM64 BPF JIT requires one CPU')
        out = out.with_name(out.name + '-bpf-jit')
    # cp -R 保留顶层符号链接；补丁只能写入独立克隆目录。
    base = (WORK / 'lkl-source').resolve(strict=True)
    source = out / 'source'
    build_dir = out / 'build'
    # 输出目录检查先于创建、删除和复制，祖先链接也可能改变实际写入位置。
    if any(path.is_symlink() or not path.resolve().is_relative_to(WORK)
           for path in (out, source, build_dir)):
        raise RuntimeError('独立内核源码与构建输出需要本树 WORK 内的独立目录')
    source_device = probe_lkl.require_case_sensitive_directory(base)
    if any(probe_lkl.require_case_sensitive_directory(path) != source_device for path in (out, source, build_dir)):
        raise RuntimeError('APFS 克隆需要 Linux 源码与构建目录位于同一文件系统')
    out.mkdir(parents=True, exist_ok=True)
    if (base / '.droidondarwin-source-revision').read_text().strip() != probe_lkl.REVISION:
        raise RuntimeError('需要先准备当前单核 LKL 源码')
    bpf_inputs = []
    bpf_source_files = []
    if bpf_jit:
        import lkl_bpf_jit_patch
        lkl_bpf_jit_patch.check_sources(base, lkl_bpf_jit_patch.UPSTREAM_SHA256)
        bpf_inputs = [WORK / 'lkl-source' / name for name in lkl_bpf_jit_patch.UPSTREAM_SHA256]
        bpf_source_files = [source / name for name in
                            (*lkl_bpf_jit_patch.UPSTREAM_SHA256, *lkl_bpf_jit_patch.GENERATED_FILES)]
    # 复制文件与补丁脚本共同决定生成源码，缺少任一来源会复用过期内核。
    copied = [EXP / name for name, _ in lkl_nce_patch.COPIED_SOURCES]
    patch_files = [Path(__file__), Path(probe_lkl.__file__), Path(lkl_translation_patch.__file__),
                   Path(lkl_ashmem_patch.__file__), *lkl_ashmem_patch.input_files(),
                   ROOT / 'scripts/lkl_nce_patch.py', ROOT / 'scripts/lkl_translation_aslr_patch.py', *copied,
                   EXP / 'translation/kernel-api.h', EXP / 'direct/page-table-layout.h']
    if guest_page_bytes == 4096:
        patch_files.append(ROOT / 'scripts/lkl_4k_vmalloc_patch.py')
    if bpf_jit:
        patch_files += [ROOT / 'scripts/lkl_bpf_jit_patch.py', EXP / 'lkl-bpf-jit-memory.c', *bpf_inputs]
    if cpus == 2:
        patch_files += [ROOT / ('scripts/' + name + '.py') for name in
                        ('lkl_smp_patch', 'lkl_smp_mmu_patch', 'lkl_translation_smp_patch')]
        patch_files += sorted((EXP / 'smp').glob('*.[ch]'))
    patch_digest = hashlib.sha256(str(guest_page_bytes).encode() + b''.join(path.read_bytes() for path in patch_files)).hexdigest()
    stamp = out / 'source-inputs.sha256'
    prepared = source.exists() and stamp.exists() and stamp.read_text().strip() == patch_digest
    generated_source_files = [source / name for name in lkl_ashmem_patch.GENERATED_FILES] + bpf_source_files
    if prepared:
        # 复用源码前核对保存的生成文件摘要。
        try:
            hashes = json.loads((out / 'build.json').read_text())['sha256']
            prepared = all(path.is_file() and hashes.get(str(path.relative_to(ROOT))) ==
                           hashlib.sha256(path.read_bytes()).hexdigest() for path in generated_source_files)
        except (OSError, KeyError, json.JSONDecodeError):
            prepared = False
    if not prepared:
        # This directory contains only generated sources; build objects stay separate.
        if source.exists():
            shutil.rmtree(source)
        if not source.exists():
            probe_lkl.run(['/bin/cp', '-cR', base, source], timeout=120)
        if not source.is_dir() or source.samefile(base):
            raise RuntimeError('独立内核源码目录与原始输入没有分离')
        if (source / '.droidondarwin-source-revision').read_text().strip() != probe_lkl.REVISION:
            raise RuntimeError('独立内核源码版本不匹配，需要重新准备该构建目录')
        probe_lkl.apply(source, EXP)
        if cpus == 2:
            import lkl_smp_patch
            lkl_smp_patch.apply(source, EXP, tests=False)
        lkl_translation_patch.apply(source, EXP, guest_page_bytes)
        if bpf_jit:
            lkl_bpf_jit_patch.apply(source, EXP)
        if cpus == 2:
            import lkl_smp_mmu_patch
            import lkl_translation_smp_patch
            lkl_smp_mmu_patch.apply(source, EXP, tests=False)
            lkl_translation_smp_patch.apply(source, EXP)
        stamp.write_text(patch_digest + '\n')
    build_dir.mkdir(exist_ok=True)
    config = WORK / ('lkl-process-build/.config' if cpus == 1 else 'lkl-smp-process-build/.config')
    if ('CONFIG_SMP=y' in config.read_text()) != (cpus == 2):
        raise RuntimeError('翻译内核配置与 CPU 数量不一致')
    requested_config, count = re.subn(r'^(?:# CONFIG_HIGH_RES_TIMERS is not set|CONFIG_HIGH_RES_TIMERS=[yn])$',
                                     'CONFIG_HIGH_RES_TIMERS=y' if high_res_timers else '# CONFIG_HIGH_RES_TIMERS is not set',
                                     config.read_text(), flags=re.MULTILINE)
    if count != 1:
        raise RuntimeError('基础内核配置缺少唯一的 HIGH_RES_TIMERS 项')
    if not (build_dir / '.config').exists() or (build_dir / '.config').read_text() != requested_config:
        (build_dir / '.config').write_text(requested_config)
    llvm = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    clang = subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
    target = [llvm / 'clang', '--target=aarch64-linux-gnu', '-ffixed-x18', '-mno-outline-atomics',
              f'-DNCE_GUEST_PAGE_SHIFT={guest_page_bytes.bit_length() - 1}']
    env = os.environ.copy()
    env.update(KBUILD_BUILD_TIMESTAMP='1970-01-01 00:00:00 UTC', KBUILD_BUILD_VERSION='1',
               KBUILD_BUILD_USER='droidondarwin', KBUILD_BUILD_HOST='local')
    sed = subprocess.check_output(['brew', '--prefix', 'gnu-sed'], text=True).strip()
    env['PATH'] = f'{sed}/libexec/gnubin:{llvm}:' + env['PATH']
    host = [clang, '-isysroot', sdk, '-I', str(WORK / 'processes/host-include'),
            '-idirafter', str(WORK / 'processes/linux-uapi/include')]
    make = ['gmake', '-C', source, f'O={build_dir}', 'ARCH=lkl', 'CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu',
            'CC=' + shlex.join(map(str, target)), 'HOSTCC=' + shlex.join(host), 'LD=' + shutil.which('ld.lld'),
            'AR=llvm-ar', 'NM=llvm-nm', 'OBJCOPY=llvm-objcopy', 'OBJDUMP=llvm-objdump', 'STRIP=llvm-strip']
    commands = []

    def run(command, log):
        commands.append(list(map(str, command)))
        probe_lkl.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600)

    with (out / 'build.log').open('w') as log:
        minimum, maximum = {16384: (16, 24), 4096: (18, 26)}[guest_page_bytes]
        run([source / 'scripts/config', '--file', build_dir / '.config',
             *[argument for name in probe_lkl.BLOCK_OPTIONS for argument in ('-e', name)],
             '-e', 'BINFMT_SCRIPT',
             '-e', 'SHMEM', '-e', 'ASHMEM',
             *(['-e', 'LKL_BPF_JIT', '-e', 'BPF_JIT', '-d', 'BPF_JIT_ALWAYS_ON'] if bpf_jit else []),
             '--set-val', 'ARCH_MMAP_RND_BITS', str(maximum)], log)
        run([*make, 'olddefconfig'], log)
        if any('CONFIG_' + name + '=y\n' not in (build_dir / '.config').read_text()
               for name in (*probe_lkl.BLOCK_OPTIONS, 'SHMEM', 'ASHMEM')):
            raise RuntimeError('生成的翻译内核配置缺少 Android 块设备、system DMA heap 或 ashmem 支持')
        if ('CONFIG_HIGH_RES_TIMERS=y' in (build_dir / '.config').read_text()) != high_res_timers:
            raise RuntimeError('生成的内核配置与请求的定时器变体不一致')
        if 'CONFIG_BINFMT_SCRIPT=y\n' not in (build_dir / '.config').read_text():
            raise RuntimeError('生成的内核配置缺少原 Android 脚本的执行支持')
        if bpf_jit and any('CONFIG_' + name + '=y\n' not in (build_dir / '.config').read_text()
                           for name in ('LKL_BPF_JIT', 'HAVE_EBPF_JIT', 'BPF_JIT', 'BPF_JIT_DEFAULT_ON')):
            raise RuntimeError('生成的内核配置缺少请求的 ARM64 BPF JIT')
        aslr = ('ARCH_WANT_DEFAULT_TOPDOWN_MMAP_LAYOUT=y', 'HAVE_ARCH_MMAP_RND_BITS=y',
                f'ARCH_MMAP_RND_BITS_MIN={minimum}', f'ARCH_MMAP_RND_BITS_MAX={maximum}',
                f'ARCH_MMAP_RND_BITS={maximum}')
        if any('CONFIG_' + value + '\n' not in (build_dir / '.config').read_text() for value in aslr):
            raise RuntimeError('生成的翻译内核配置与 mmap 随机位数不一致')
        run([*make, '-j8', 'all'], log)
        dependency_command, ashmem_inputs = lkl_ashmem_patch.compiled_inputs(build_dir)
        commands.append(dependency_command)
        shutil.copyfile(source / 'arch/lkl/include/uapi/asm/host_ops.h', out / 'lkl-host-ops.h')
        run([*target, '-fPIC', '-fno-stack-protector', '-O2', '-I', out, '-I', EXP,
             '-c', EXP / 'lkl-bridge.c', '-o', out / 'lkl-bridge.o'], log)
        exports = ('vsnprintf', 'lkl_ops', 'sysctl_sched_base_slice', 'nce_run_syscall', 'nce_page_fault',
                   'nce_set_layout', 'nce_user_return', 'nce_user_fault', 'nce_user_preempt', 'nce_prepare_user',
                   'nce_translate_user')
        if cpus == 2:
            exports += ('nce_translation_enter', 'nce_translation_leave', 'nce_translation_exclusive', 'nce_window_unlock', 'nce_translation_written')
        run([llvm / 'llvm-objcopy', *(['--set-section-alignment=.data..percpu=16384'] if cpus == 2 else []),
             *['--globalize-symbol=' + name for name in exports],
             build_dir / 'lkl.o', out / 'lkl-exports.o'], log)
        run([shutil.which('ld.lld'), '-shared', '-Bsymbolic', '--no-undefined', '-z', 'max-page-size=16384',
             '-z', 'common-page-size=16384', out / 'lkl-exports.o', out / 'lkl-bridge.o', '-o', out / 'lkl.so'], log)
    probe_lkl.OUT = out
    probe_lkl.PROCESS = True
    probe_lkl.SMP = cpus == 2
    probe_lkl.package_elf(out / 'lkl.so', smp_exports={'nce_translation_enter', 'nce_translation_leave', 'nce_translation_exclusive', 'nce_window_unlock', 'nce_translation_written'})
    files = [Path(__file__), Path(lkl_translation_patch.__file__), Path(probe_lkl.__file__),
             Path(lkl_ashmem_patch.__file__), *lkl_ashmem_patch.input_files(),
             *generated_source_files, *ashmem_inputs,
             ROOT / 'scripts/lkl_nce_patch.py', ROOT / 'scripts/lkl_translation_aslr_patch.py', *copied, EXP / 'translation/kernel-api.h',
             *[source / 'arch/lkl' / destination for _, destination in lkl_nce_patch.COPIED_SOURCES],
             source / 'arch/lkl/kernel/Makefile',
             EXP / 'lkl-bridge.c', EXP / 'direct/page-table-layout.h', config, build_dir / '.config',
             source / 'arch/lkl/include/asm/page-mmu.h', source / 'arch/lkl/include/asm/nce-layout.h',
             source / 'arch/lkl/mm/nce_mmu.c', source / 'arch/lkl/kernel/nce_elf.c',
             source / 'arch/lkl/include/asm/pgtable-mmu-3level.h', source / 'arch/lkl/include/asm/nce-translation.h',
             source / 'arch/lkl/include/asm/mmu.h', source / 'arch/lkl/kernel/threads.c',
             source / 'arch/lkl/kernel/nce_process.c', source / 'arch/lkl/kernel/syscalls.c',
             source / 'drivers/md/dm-ioctl.c',
             source / 'arch/lkl/kernel/setup.c',
             source / 'arch/lkl/Kconfig', source / 'arch/lkl/include/asm/processor.h',
             source / 'arch/lkl/include/asm/elf.h', source / 'fs/binfmt_elf.c',
             source / 'mm/util.c', source / 'mm/mmap.c', source / 'kernel/sysctl.c',
             *[out / name for name in ('lkl.so', 'lkl-image.bin', 'lkl-image.h', 'lkl-host-ops.h')]]
    if guest_page_bytes == 4096:
        files += [ROOT / 'scripts/lkl_4k_vmalloc_patch.py', source / 'mm/vmalloc.c', source / 'mm/memory.c', source / 'arch/lkl/include/asm/vmalloc.h', source / 'kernel/bpf/ringbuf.c', source / 'fs/erofs/zutil.c']
    if bpf_jit:
        files += [ROOT / 'scripts/lkl_bpf_jit_patch.py', EXP / 'lkl-bpf-jit-memory.c', *bpf_inputs, *bpf_source_files]
    if cpus == 2:
        files += [ROOT / ('scripts/' + name + '.py') for name in ('lkl_smp_patch', 'lkl_smp_mmu_patch', 'lkl_translation_smp_patch')]
        files += list((EXP / 'smp').glob('*.[ch]'))
    manifest = {'cpus': cpus, 'guest_page_bytes': guest_page_bytes, 'host_page_bytes': 16384, 'high_res_timers': high_res_timers, 'bpf_jit': bpf_jit, 'status': 'built; functional validation belongs to probe_lkl_translation.py',
                'build_identity': {key: env[key] for key in env if key.startswith('KBUILD_BUILD_')},
                'revision': probe_lkl.REVISION, 'commands': commands,
                'sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (out / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return out


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpus', type=int, choices=(1, 2), default=1)
    parser.add_argument('--high-res-timers', action='store_true')
    parser.add_argument('--guest-page-bytes', type=int, choices=(4096, 16384), default=16384)
    parser.add_argument('--bpf-jit', action='store_true')
    args = parser.parse_args()
    print(build(args.cpus, args.high_res_timers, args.guest_page_bytes, args.bpf_jit))
