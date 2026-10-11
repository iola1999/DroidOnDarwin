#!/usr/bin/env python3
"""检查高于 Linux 用户窗口的宿主字符串；可对旧内核验证 EFAULT。"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path, default=ROOT / 'work/native-linux-nce/lkl-translation-kernel')
parser.add_argument('--expect-failure', action='store_true')
args = parser.parse_args()
kernel = args.kernel.resolve()
out = ROOT / 'work/native-linux-nce/host-string-regression' / ('control' if args.expect_failure else 'fixed')
out.mkdir(parents=True, exist_ok=True)
source = ROOT / 'tests/performance/host-string-access.c'
exp = ROOT / 'experiments/native-linux'
binary = out / 'probe'
command = ['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(kernel), '-I', str(exp),
           str(source), str(exp / 'translation/kernel-image.c'), str(exp / 'lkl-host.c'), '-o', str(binary)]
subprocess.run(command, check=True, timeout=60)
entitlements = out / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'com.apple.security.cs.allow-unsigned-executable-memory': True}))
subprocess.run(['codesign', '--force', '--sign', '-', '--options', 'runtime', '--entitlements', str(entitlements), str(binary)], check=True)
result = subprocess.run([str(binary), str(kernel / 'lkl-image.bin'), 'failed' if args.expect_failure else 'passed'], text=True, capture_output=True, timeout=30)
(out / 'probe.log').write_text(result.stdout + result.stderr)
paths = [source, Path(__file__), exp / 'translation/kernel-image.c', exp / 'lkl-host.c', kernel / 'lkl-image.h', kernel / 'lkl-image.bin', out / 'probe.log']
report = {'returncode': result.returncode, 'expected_failure': args.expect_failure, 'command': command,
          'sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}}
(out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(result.stdout, result.stderr)
raise SystemExit(result.returncode != 0)
