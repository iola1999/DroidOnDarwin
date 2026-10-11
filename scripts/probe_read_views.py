#!/usr/bin/env python3
"""验证独立的只读 RAM 别名与回收，记录阶段内存；尚未接入 Linux/Direct。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=WORK / 'read-views')
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(WORK.resolve()):
        parser.error('产物必须位于 work/native-linux-nce 下')
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('需要 Apple Silicon Mac')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'summary.json').unlink(missing_ok=True)
    base = ROOT / 'experiments/native-linux'
    sources = [base / 'read-view-probe.c', base / 'translation/read-view.c', base / 'translation/linux-memory.c']
    executable = out / 'probe'
    flags = ['-O2', '-g', '-Wall', '-Wextra', '-Werror']
    command = ['xcrun', 'clang', *flags, *map(str, sources), '-o', str(executable)]
    subprocess.run(command, check=True, timeout=120)
    files = [*sources, base / 'translation/read-view.h', base / 'translation/linux-memory.h',
             Path(__file__), executable]
    hashes = {str(path.relative_to(ROOT)): sha(path) for path in files}
    expected = {'read_views_private_shared_and_last_page', 'read_views_context_selection_keeps_mappings',
                'read_views_replace_revoke_and_write_protection', 'read_views_contiguous_range_and_partial_revoke',
                'read_views_invalid_ranges_preserve_mappings', 'read_views_destroy_recreate_and_cleanup'}
    runs = []
    samples = 0
    for entry in ('cli', 'gcd'):
        stdout, stderr = out / f'{entry}.jsonl', out / f'{entry}.log'
        with stdout.open('w') as output, stderr.open('w') as error:
            process = subprocess.run([executable, entry], stdout=output, stderr=error, timeout=30)
        rows = [json.loads(line) for line in stdout.read_text().splitlines() if line.startswith('{')]
        passed = process.returncode == 0 and rows and rows[-1].get('result') == 'passed'
        passed = passed and {r['test'] for r in rows if r.get('passed')} == expected
        summaries = [r for r in rows if 'memory_statistics' in r]
        passed = passed and {r['memory_statistics'] for r in summaries} == {'kernel_setup', 'reserve', 'aliases', 'cleanup'}
        for summary in summaries:
            values = [r for r in rows if r.get('memory_sample') == summary['memory_statistics']]
            passed = passed and len(values) == summary['samples'] and not summary['errors']
            samples += len(values)
            for metric in ('footprint', 'resident'):
                numbers = [r[metric + '_bytes'] for r in values]
                passed = passed and bool(numbers) and abs(sum(numbers) / len(numbers) - summary[metric + '_mean_bytes']) < .001
                passed = passed and max(numbers, default=-1) == summary[metric + '_sample_peak_bytes']
                passed = passed and bool(numbers) and numbers[-1] == summary[metric + '_latest_bytes']
        runs.append({'entry': entry, 'returncode': process.returncode, 'validated': bool(passed), 'results': rows,
                     'stdout_sha256': sha(stdout), 'stderr_sha256': sha(stderr)})
        if not passed:
            (out / 'failed.json').write_text(json.dumps(runs, indent=2) + '\n')
            raise SystemExit(f'检查失败：{stdout}')
    for path, digest in hashes.items():
        if sha(ROOT / path) != digest:
            raise RuntimeError('检查期间输入发生变化：' + path)
    report = {'scope': 'Mac-only two 4 GiB read-only data aliases; no Linux/Direct/iPhone integration or performance claim',
              'platform': platform.platform(), 'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'build': command, 'sha256': hashes, 'runs': runs, 'recomputed_memory_samples': samples}
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
