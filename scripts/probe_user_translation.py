#!/usr/bin/env python3
"""比较相同 ARM64 负载的原生执行与 Unicorn 软件 TLB 执行。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'experiments/native-linux/translation'
OUT = ROOT / 'work/native-linux-nce/user-translation'
PYTHON = ROOT / 'work/native-linux-nce/user-translation-venv/bin/python'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('该原型需要 Apple Silicon Mac')
    if not PYTHON.is_file():
        parser.error(f'先在 {PYTHON.parent.parent} 创建 venv 并安装 unicorn==2.1.4')
    dependency = json.loads(subprocess.check_output([PYTHON, '-c',
        'import json, pathlib, unicorn; print(json.dumps({"version":unicorn.__version__, "path":str(pathlib.Path(unicorn.__file__).parent)}))'], text=True))
    if dependency['version'] != '2.1.4':
        parser.error('该实验固定使用 unicorn==2.1.4')
    library = Path(dependency['path'])
    OUT.mkdir(parents=True, exist_ok=True)
    executable = OUT / 'benchmark'
    command = ['xcrun', 'clang', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
               '-I', str(library / 'include'), str(SOURCE / 'benchmark.c'), str(SOURCE / 'kernels.S'),
               str(library / 'lib/libunicorn.a'), '-o', str(executable)]
    subprocess.run(command, check=True, timeout=120)
    if args.build_only:
        print(executable)
        return
    with (OUT / 'results.jsonl').open('w') as out, (OUT / 'stderr.log').open('w') as err:
        result = subprocess.run([executable], stdout=out, stderr=err, timeout=120)
    rows = [json.loads(line) for line in (OUT / 'results.jsonl').read_text().splitlines()]
    files = [SOURCE / 'benchmark.c', SOURCE / 'kernels.S', Path(__file__), executable, library / 'lib/libunicorn.a']
    report = {'scope': 'Independent ARM64 user-code translation prototype; no LKL, Android or iPhone integration',
              'platform': platform.platform(), 'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'dependency': dependency, 'command': command, 'returncode': result.returncode,
              'measurement': 'Same assembly bytes; warmed translated blocks; three alternating rounds. Wall and thread CPU include one engine entry and register exchange. Host buffer initialization and comparison excluded. Memory values are boundary samples, with lifetime resident peak; no time-weighted average.',
              'sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
              'results': rows, 'comparison': {}}
    for name in ('integer', 'memory', 'atomic'):
        values = {}
        for engine in ('native', 'unicorn'):
            samples = [r['wall_ns'] for r in rows if r.get('kernel') == name and r.get('engine') == engine]
            if samples:
                values[engine] = {'wall_ns': samples, 'median_wall_ns': statistics.median(samples)}
        if set(values) == {'native', 'unicorn'}:
            values['translated_to_native_wall_ratio'] = values['unicorn']['median_wall_ns'] / values['native']['median_wall_ns']
        report['comparison'][name] = values
    (OUT / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(OUT / 'summary.json')
    if result.returncode or not rows or rows[-1].get('result') != 'passed':
        raise SystemExit('原型检查失败；查看 results.jsonl 和 stderr.log')


if __name__ == '__main__':
    main()
