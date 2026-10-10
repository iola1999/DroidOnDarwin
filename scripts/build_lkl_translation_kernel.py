#!/usr/bin/env python3
"""构建独立的 PTE 翻译实验内核，保留产品内核产物。"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

import lkl_translation_patch
import probe_lkl

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'
EXP = ROOT / 'experiments/native-linux'
OUT = WORK / 'lkl-translation-kernel'


def build():
    base = WORK / 'lkl-source'
    source = OUT / 'source'
    build_dir = OUT / 'build'
    OUT.mkdir(parents=True, exist_ok=True)
    if (base / '.droidondarwin-source-revision').read_text().strip() != probe_lkl.REVISION:
        raise RuntimeError('需要先准备当前单核 LKL 源码')
    if not source.exists():
        probe_lkl.run(['/bin/cp', '-cR', base, source], timeout=120)
    if (source / '.droidondarwin-source-revision').read_text().strip() != probe_lkl.REVISION:
        raise RuntimeError('独立内核源码版本不匹配，需要重新准备该构建目录')
    probe_lkl.apply(source, EXP)
    lkl_translation_patch.apply(source, EXP)
    build_dir.mkdir(exist_ok=True)
    config = WORK / 'lkl-process-build/.config'
    if 'CONFIG_SMP=y' in config.read_text():
        raise RuntimeError('翻译原型需要单核内核配置')
    if not (build_dir / '.config').exists() or (build_dir / '.config').read_bytes() != config.read_bytes():
        shutil.copyfile(config, build_dir / '.config')
    llvm = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    clang = subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
    target = [llvm / 'clang', '--target=aarch64-linux-gnu', '-ffixed-x18']
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

    with (OUT / 'build.log').open('w') as log:
        run([*make, 'olddefconfig'], log)
        run([*make, '-j8', 'all'], log)
        shutil.copyfile(source / 'arch/lkl/include/uapi/asm/host_ops.h', OUT / 'lkl-host-ops.h')
        run([*target, '-fPIC', '-fno-stack-protector', '-O2', '-I', OUT, '-I', EXP,
             '-c', EXP / 'lkl-bridge.c', '-o', OUT / 'lkl-bridge.o'], log)
        exports = ('vsnprintf', 'lkl_ops', 'sysctl_sched_base_slice', 'nce_run_syscall', 'nce_page_fault',
                   'nce_set_layout', 'nce_user_return', 'nce_user_fault', 'nce_user_preempt', 'nce_prepare_user',
                   'nce_translate_user')
        run([llvm / 'llvm-objcopy', *['--globalize-symbol=' + name for name in exports],
             build_dir / 'lkl.o', OUT / 'lkl-exports.o'], log)
        run([shutil.which('ld.lld'), '-shared', '-Bsymbolic', '--no-undefined', '-z', 'max-page-size=16384',
             '-z', 'common-page-size=16384', OUT / 'lkl-exports.o', OUT / 'lkl-bridge.o', '-o', OUT / 'lkl.so'], log)
    probe_lkl.OUT = OUT
    probe_lkl.PROCESS = True
    probe_lkl.SMP = False
    probe_lkl.package_elf(OUT / 'lkl.so')
    files = [Path(__file__), Path(lkl_translation_patch.__file__), Path(probe_lkl.__file__),
             ROOT / 'scripts/lkl_nce_patch.py', EXP / 'lkl-nce-mmu.c', EXP / 'translation/kernel-api.h',
             EXP / 'lkl-bridge.c', config, build_dir / '.config',
             source / 'arch/lkl/mm/nce_mmu.c', *[OUT / name for name in ('lkl.so', 'lkl-image.bin', 'lkl-image.h', 'lkl-host-ops.h')]]
    manifest = {'status': 'built; functional validation belongs to probe_lkl_translation.py',
                'build_identity': {key: env[key] for key in env if key.startswith('KBUILD_BUILD_')},
                'revision': probe_lkl.REVISION, 'commands': commands,
                'sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (OUT / 'build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return OUT


if __name__ == '__main__':
    print(build())
