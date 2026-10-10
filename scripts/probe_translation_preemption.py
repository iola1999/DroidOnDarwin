#!/usr/bin/env python3
"""验证翻译块边界停止，并用原版入口复现指令状态不一致。"""
import hashlib
import json
import subprocess
from pathlib import Path

from build_translation_engine import build, ROOT, WORK


def main():
    dependency = build()
    patched = Path(dependency['path'])
    stock = Path(subprocess.check_output([WORK / 'user-translation-venv/bin/python', '-c',
        'import pathlib,unicorn; assert unicorn.__version__=="2.1.4"; print(pathlib.Path(unicorn.__file__).parent)'], text=True).strip())
    out = WORK / 'translation-preemption'
    out.mkdir(exist_ok=True)
    (out / 'summary.json').unlink(missing_ok=True)
    source = ROOT / 'experiments/native-linux/translation/preemption-test.c'
    results = []
    hashes = {str(source.relative_to(ROOT)): hashlib.sha256(source.read_bytes()).hexdigest()}
    for mode, prefix in [('stock', stock), ('patched', patched), ('linux-stop', patched),
                         ('checked-fault', patched), ('linux-fault', patched)]:
        command = ['xcrun', 'clang', '-O2', '-g', '-Wall', '-Wextra', '-Werror', '-I', prefix / 'include',
                   source, prefix / 'lib/libunicorn.a', '-o', out / mode]
        if mode == 'stock':
            command += ['-DTEST_STOCK_STOP']
        if mode.startswith('linux-'):
            command += ['-DTEST_LINUX_MEMORY']
        if mode.endswith('-fault'):
            command += ['-DTEST_MEMORY_FAULT']
        subprocess.run(list(map(str, command)), check=True, timeout=120)
        for iteration in range(1 if mode == 'stock' else 20):
            process = subprocess.run([out / mode], capture_output=True, text=True, timeout=10)
            rows = [json.loads(line) for line in process.stdout.splitlines()]
            if len(rows) != 4 or process.returncode != (1 if mode == 'stock' else 0):
                raise RuntimeError(mode + ': unexpected exit: ' + process.stderr + process.stdout)
            if any(row['passed'] != (mode != 'stock') for row in rows):
                raise RuntimeError(mode + ': unexpected instruction state')
            results.append({'mode': mode, 'iteration': iteration, 'returncode': process.returncode, 'checks': rows})
        for path in (prefix / 'lib/libunicorn.a', out / mode):
            hashes[str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    hashes[str(Path(__file__).relative_to(ROOT))] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report = {'scope': 'Deterministic stop request from a successful data TLB fill; four ARM64 memory instructions',
              'dependency': dependency, 'sha256': hashes, 'runs': results}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
