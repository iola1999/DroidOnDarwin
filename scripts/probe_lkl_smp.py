#!/usr/bin/env python3
"""构建独立的双 CPU LKL 内核，验证定向中断、计算重叠和任务迁移。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

import probe_lkl
from lkl_nce_patch import apply as apply_nce
from lkl_smp_patch import apply as apply_smp
from lkl_smp_mmu_patch import apply as apply_mmu

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
OUT = ROOT / 'work/native-linux-nce/smp'
REVISION = probe_lkl.REVISION
run = probe_lkl.run


def main():
    global OUT
    parser = argparse.ArgumentParser()
    parser.add_argument('--mmu', action='store_true', help='Validate shared-mm RAM access in the SMP kernel')
    parser.add_argument('--core-scheduling', action='store_true', help='Coordinate the two CPUs by address space')
    arguments = parser.parse_args()
    if arguments.core_scheduling and not arguments.mmu:
        parser.error('--core-scheduling requires --mmu')
    if arguments.mmu:
        OUT = OUT.with_name('smp-mmu-core' if arguments.core_scheduling else 'smp-mmu')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise SystemExit('需要 Apple Silicon Mac')
    OUT.mkdir(parents=True, exist_ok=True)
    inputs = [Path(__file__), Path(__file__).with_name('lkl_smp_patch.py'),
              Path(__file__).with_name('lkl_nce_patch.py'), Path(__file__).with_name('lkl_smp_mmu_patch.py'),
              Path(__file__).with_name('probe_lkl.py'), *sorted((EXP / 'smp').glob('*')),
              *[EXP / name for name in ['lkl-main.c', 'lkl-host.c', 'lkl-bridge.c', 'dispatch-test.c',
                                       'nce.c', 'nce-thread.c', 'nce.h', 'nce-thread.h', 'entry.S', 'layout.h']]]
    if arguments.mmu:
        inputs += [*sorted(EXP.glob('lkl-nce-*.c')), *sorted(EXP.glob('lkl-*.h')),
                   EXP / 'guest-registers.h']
    if arguments.core_scheduling:
        inputs.append(Path(__file__).with_name('lkl_smp_core_patch.py'))
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    input_hashes = {str(p.relative_to(ROOT)): digest(p) for p in inputs}
    source = OUT / 'source'
    build = OUT / 'build'
    if arguments.mmu and not source.exists():
        base = ROOT / 'work/native-linux-nce/lkl-source'
        assert (base / '.droidondarwin-source-revision').read_text().strip() == REVISION
        run(['/bin/cp', '-cR', base, source], timeout=120)
    if not source.exists():
        archive = OUT / 'source.tar.gz'
        urllib.request.urlretrieve(f'https://codeload.github.com/lkl/linux/tar.gz/{REVISION}', archive)
        with tarfile.open(archive) as tar:
            tar.extractall(OUT, filter='data')
        (OUT / f'linux-{REVISION}').rename(source)
        (source / '.droidondarwin-source-revision').write_text(REVISION + '\n')
        archive.unlink()
    if (source / '.droidondarwin-source-revision').read_text().strip() != REVISION:
        raise SystemExit('LKL 源码版本不匹配')
    # Use an independent source/build tree; the desktop image retains UP/MMU.
    base_marker = source / '.droidondarwin-smp-base'
    base_hash = hashlib.sha256(Path(__file__).with_name('lkl_nce_patch.py').read_bytes()).hexdigest()
    if not base_marker.exists():
        apply_nce(source, EXP)
        base_marker.write_text(base_hash + '\n')
    elif base_marker.read_text().strip() != base_hash:
        raise SystemExit('NCE 基础补丁已变化，请重新生成独立 SMP 源码树')
    apply_smp(source, EXP)
    if arguments.mmu:
        apply_mmu(source, EXP)
    if arguments.core_scheduling:
        from lkl_smp_core_patch import apply as apply_core
        apply_core(source, EXP)
    mod = source / 'scripts/mod/file2alias.c'
    text = mod.read_text()
    if '#define uuid_t lkl_mod_uuid_t' not in text:
        mod.write_text(text.replace('#include <stdbool.h>',
                                   '#include <stdbool.h>\n#ifdef __APPLE__\n#define uuid_t lkl_mod_uuid_t\n#endif'))
    llvm = Path(subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    clang = subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
    sed = subprocess.check_output(['brew', '--prefix', 'gnu-sed'], text=True).strip()
    env = {**os.environ, 'PATH': f'{sed}/libexec/gnubin:{llvm}:' + os.environ['PATH']}
    include = ROOT / 'work/native-linux-nce/processes/host-include'
    target = [llvm / 'clang', '--target=aarch64-linux-gnu', '-ffixed-x18', '-mno-outline-atomics']
    make = ['gmake', '-C', source, f'O={build}', 'ARCH=lkl', 'CLANG_TARGET_FLAGS_lkl=aarch64-linux-gnu',
            'CC=' + shlex.join(map(str, target)),
            'HOSTCC=' + shlex.join([clang, '-isysroot', sdk, '-I', str(include)]),
            'LD=' + shutil.which('ld.lld'), 'AR=llvm-ar', 'NM=llvm-nm', 'OBJCOPY=llvm-objcopy',
            'OBJDUMP=llvm-objdump', 'STRIP=llvm-strip']
    build.mkdir(exist_ok=True)
    with (OUT / 'build.log').open('w') as log:
        if not (build / '.config').exists():
            run([*make, 'allnoconfig'], env=env, stdout=log, stderr=subprocess.STDOUT)
        enabled = ['64BIT', 'EXPERT', 'PRINTK', 'PROC_FS', 'SYSFS', 'FUTEX', 'EPOLL', 'SIGNALFD',
                   'TIMERFD', 'SMP', 'DEBUG_KERNEL', 'LKL_HOST_MEMCPY', 'LKL_HOST_MEMSET', 'LKL_HOST_MEMMOVE']
        if arguments.mmu:
            enabled += ['MMU']
        if arguments.core_scheduling:
            enabled += ['SCHED_SMT', 'SCHED_CORE']
        switches = [item for name in enabled for item in ('-e', name)]
        switches += [item for name in ['BLOCK', 'KALLSYMS', 'DEBUG_INFO', 'SLUB_DEBUG',
                                       *([] if arguments.mmu else ['MMU'])] for item in ('-d', name)]
        run([source / 'scripts/config', '--file', build / '.config', *switches], env=env)
        run([*make, 'olddefconfig'], env=env, stdout=log, stderr=subprocess.STDOUT)
        config = (build / '.config').read_text()
        if ('CONFIG_SMP=y\n' not in config or 'CONFIG_NR_CPUS=2\n' not in config or
                ('CONFIG_MMU=y\n' in config) != arguments.mmu):
            raise SystemExit('双 CPU 原型的 SMP/MMU 配置不匹配')
        if arguments.mmu:
            assert 'CONFIG_MMU_GATHER_RCU_TABLE_FREE=y\n' in config
            assert 'CONFIG_ARCH_HAS_PTE_SPECIAL=y\n' in config
        if arguments.core_scheduling:
            assert 'CONFIG_SCHED_SMT=y\n' in config and 'CONFIG_SCHED_CORE=y\n' in config
        run([*make, '-j8', 'all'], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=900)
    shutil.copyfile(source / 'arch/lkl/include/uapi/asm/host_ops.h', OUT / 'lkl-host-ops.h')
    shutil.copyfile(ROOT / 'work/native-linux-nce/guest-image.h', OUT / 'guest-image.h')
    run([*target, '-fPIC', '-fno-stack-protector', '-O2', '-I', OUT, '-I', EXP,
         '-c', EXP / 'lkl-bridge.c', '-o', OUT / 'bridge.o'])
    # LKL is linked again as a shared image; preserve the percpu template's
    # page alignment after the kernel's intermediate relocatable link.
    exports = ['nce_run_syscall', 'nce_page_fault', 'nce_set_layout', 'nce_user_return',
               'nce_user_fault', 'nce_user_preempt', 'nce_prepare_user'] if arguments.mmu else []
    run([llvm / 'llvm-objcopy', '--globalize-symbol=vsnprintf', '--globalize-symbol=lkl_ops',
         *['--globalize-symbol=' + name for name in exports],
         f'--set-section-alignment=.data..percpu={16384 if arguments.mmu else 4096}',
         build / 'lkl.o', OUT / 'exports.o'])
    run(['ld.lld', '-shared', '-Bsymbolic', '--no-undefined', '-z', 'max-page-size=16384',
         '-z', 'common-page-size=16384', OUT / 'exports.o', OUT / 'bridge.o', '-o', OUT / 'lkl.so'])
    probe_lkl.OUT, probe_lkl.PROCESS = OUT, arguments.mmu
    probe_lkl.package_elf(OUT / 'lkl.so')
    host_source = (EXP / 'lkl-main.c').read_text()
    assert host_source.count('    blocking_tests();') == 1
    host_source = host_source.replace('    blocking_tests();',
                                     f'    blocking_tests();\n    REQUIRE(kernel_syscall({242 if arguments.mmu else 244}, args) == 0);')
    if arguments.mmu:
        host_source = '#include "smp/mmu-host.h"\n' + host_source
        point = '    kernel_syscall = (void *)(kernel.rx + LKL_SYSCALL);'
        assert host_source.count(point) == 1
        host_source = host_source.replace(point, point + '\n    nce_smp_mmu_setup((void *)(kernel.rx + NCE_SET_LAYOUT));')
        host_source = host_source.replace('    cleanup();', '    cleanup();\n    nce_smp_mmu_cleanup();')
    (OUT / 'main.c').write_text(host_source)
    host = [clang, '-isysroot', sdk, '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', OUT, '-I', EXP,
            OUT / 'main.c', EXP / 'lkl-host.c', EXP / 'nce.c', EXP / 'nce-thread.c', EXP / 'entry.S']
    if arguments.mmu:
        host += [EXP / 'smp/mmu-host.c']
    run([*host, '-o', OUT / 'probe'])
    run([*host, '-DNCE_IOS_APP', EXP / 'dispatch-test.c', '-o', OUT / 'dispatch-probe'])
    runs, raw = [], ['lkl.so', 'lkl-image.bin', 'build/.config']
    required = {'linux_smp_parallel_kernel', 'linux_smp_atomic64_futex', 'linux_smp_ipi_timer_migration'}
    if arguments.mmu:
        required |= {'linux_smp_mmu_access_boundaries', 'linux_smp_mmu_atomic_and_unmap',
                     'linux_smp_shared_window', 'linux_smp_distinct_windows',
                     'linux_smp_data_mapping_without_stop', 'linux_smp_guest_present_restore'}
    for iteration in range(3):
        for entry in ['probe', 'dispatch-probe']:
            prefix = f'{entry}-{iteration}'
            output_name, error_name = prefix + '-results.jsonl', prefix + '-console.log'
            with (OUT / output_name).open('w') as output, (OUT / error_name).open('w') as error:
                run([OUT / entry, OUT / 'lkl-image.bin'], stdout=output, stderr=error, timeout=90)
            records = [json.loads(line) for line in (OUT / output_name).read_text().splitlines()]
            kernel_checks = [json.loads(line) for line in (OUT / error_name).read_text().splitlines()
                             if line.startswith('{"test":"linux_smp_')]
            assert records[-1].get('result') == 'passed'
            assert {r['test'] for r in kernel_checks if r.get('passed')} == required
            runs.append({'entry': entry, 'iteration': iteration, 'results': records, 'kernel_checks': kernel_checks})
            raw.extend([output_name, error_name])
    if input_hashes != {str(p.relative_to(ROOT)): digest(p) for p in inputs}:
        raise SystemExit('验证期间输入文件发生变化，请重新运行')
    report = {'scope': 'two CPU Linux kernel only; NCE process execution is not enabled',
              'revision': REVISION, 'runs': runs,
              'inputs': input_hashes,
              'host': {'platform': platform.platform(),
                       'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
                       'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip()},
              'configuration': {'cpus': 2, 'mmu': arguments.mmu, 'core_scheduling': arguments.core_scheduling,
                                'nce_processes': False, 'debug_kernel': True},
              'raw': {name: digest(OUT / name) for name in raw}}
    (OUT / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps([{'entry': r['entry'], 'iteration': r['iteration'], 'checks': r['kernel_checks']}
                      for r in runs], ensure_ascii=False))


if __name__ == '__main__':
    main()
