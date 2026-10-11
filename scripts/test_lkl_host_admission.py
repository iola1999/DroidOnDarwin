#!/usr/bin/env python3
"""用任务创建期间的受控阻塞检查首次宿主调用互斥；诊断内核独立于产品。"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess

import build_lkl_translation_kernel as builder

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
OUT = ROOT / 'work/native-linux-nce/host-admission-regression'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--without-serialization', action='store_true', help='仅在独立诊断内核中撤去首次创建互斥，预期检测到重入')
    args = parser.parse_args()
    out = OUT / ('control' if args.without_serialization else 'fixed')
    out.mkdir(parents=True, exist_ok=True)
    builder.OUT = out / 'kernel'
    # The injected source must be regenerated even when the product patch digest is unchanged.
    source = builder.OUT / 'source'
    if source.exists():
        marker = source / '.droidondarwin-source-revision'
        if not marker.is_file() or marker.read_text().strip() != builder.probe_lkl.REVISION:
            raise RuntimeError('诊断源码目录标识不匹配')
        shutil.rmtree(source)
    original = builder.lkl_translation_patch.apply

    def inject(source, experiment):
        original(source, experiment)
        path = source / 'arch/lkl/kernel/syscalls.c'
        text = path.read_text()
        if args.without_serialization:
            start = text.index('long lkl_syscall(')
            end = text.index('static struct task_struct *idle_host_task;', start)
            part = text[start:end]
            for line in ('\t\t\tlkl_ops->mutex_lock(host_task_lock);\n', '\t\t\tlkl_ops->mutex_unlock(host_task_lock);\n'):
                if part.count(line) != 1:
                    raise RuntimeError('首次调用互斥位置不匹配')
                part = part.replace(line, '')
            text = text[:start] + part + text[end:]
        changes = {
            'static int host_task_id;': 'static int host_task_id;\nstatic unsigned diagnostic_creators;',
            '\tswitch_to_host_task(host0);\n': '\tif (++diagnostic_creators != 1)\n\t\tlkl_bug("concurrent host0 creation\\n");\n\tswitch_to_host_task(host0);\n\tschedule_timeout_uninterruptible(1);\n',
            '\tif (pid < 0)\n\t\treturn pid;': '\tif (pid < 0) {\n\t\t--diagnostic_creators;\n\t\treturn pid;\n\t}',
            '\tsnprintf((*task)->comm, sizeof((*task)->comm), "host%d", host_task_id);': '\tsnprintf((*task)->comm, sizeof((*task)->comm), "host%d", host_task_id);\n\t--diagnostic_creators;',
        }
        for old, new in changes.items():
            if text.count(old) != 1:
                raise RuntimeError('诊断插入位置不匹配')
            text = text.replace(old, new)
        path.write_text(text)

    builder.lkl_translation_patch.apply = inject
    kernel = builder.build(1)
    executable = out / 'probe'
    command = ['xcrun', 'clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-I', str(kernel), '-I', str(EXP), str(EXP / 'host-admission-test.c'),
               str(EXP / 'translation/kernel-image.c'), str(EXP / 'lkl-host.c'), '-o', str(executable)]
    subprocess.run(command, check=True, timeout=60)
    entitlements = out / 'entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({'com.apple.security.cs.allow-unsigned-executable-memory': True}))
    subprocess.run(['codesign', '--force', '--sign', '-', '--options', 'runtime', '--entitlements', str(entitlements), str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable), str(kernel / 'lkl-image.bin')], capture_output=True, text=True, timeout=180)
    output = result.stdout + result.stderr
    (out / 'probe.log').write_text(output)
    expected = (result.returncode != 0 and 'concurrent host0 creation' in output) if args.without_serialization else (result.returncode == 0 and output.count(' passed\n') == 129)
    sources = [Path(__file__), EXP / 'host-admission-test.c', EXP / 'translation/kernel-image.c', EXP / 'lkl-host.c',
               kernel / 'source/arch/lkl/kernel/syscalls.c', kernel / 'lkl-image.bin', out / 'probe.log']
    report = {'scope': '4096 first host calls with one-jiffy sleep in shared host0 creation; diagnostic only; no graphics fault causality claim',
              'without_serialization': args.without_serialization, 'returncode': result.returncode, 'expected_outcome': expected,
              'command': command, 'sha256': {str(path.relative_to(ROOT)): digest(path) for path in sources}}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'expected_outcome': expected, 'returncode': result.returncode, 'output': str(out)}))
    return 0 if expected else 1


if __name__ == '__main__':
    raise SystemExit(main())
