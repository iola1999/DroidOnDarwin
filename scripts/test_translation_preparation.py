#!/usr/bin/env python3
"""在独立测试二进制中逐次拒绝执行内存准备，检查 CLI/GCD 返回与部分资源释放。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    config = report['configuration']
    if config['code_memory'] != 'dual' or config['workload'] != 'core' or config['execution'] != 'direct':
        parser.error('需要双映射 Direct 核心检查结果')
    if not all(run['validated'] for run in report['runs']):
        parser.error('核心检查尚未成功')
    for name, digest in report['sha256'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            parser.error('输入已变化：' + name)
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'work/native-linux-nce'):
        parser.error('输出必须放在 work/native-linux-nce')
    out.mkdir(parents=True, exist_ok=True)
    renamed = out / 'code-memory.o'
    subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Dnce_code_prepare_range=real_prepare_range', '-Dnce_code_release_range=real_release_range',
                    '-c', str(EXP / 'code-memory.c'), '-o', str(renamed)], check=True, timeout=60)
    environment = {**{key: value for key, value in os.environ.items() if not key.startswith('NCE_')}, 'NCE_TRANSLATION_CACHE_MIB': '16', 'NCE_TRANSLATION_PREEMPTION': 'timer',
                   'NCE_DIRECT_MEMORY': 'flat', 'NCE_DIRECT_NATIVE_EXCLUSIVE': '1', 'NCE_DIRECT_CROSS_PAGE': '1'}
    result = {'source_report': str(args.report), 'source_report_sha256': hashlib.sha256(args.report.read_bytes()).hexdigest(),
              'source_configuration': config, 'test_environment': {key: value for key, value in environment.items() if key.startswith('NCE_')},
              'commands': [], 'runs': [], 'sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in (Path(__file__).resolve(), EXP / 'translation/preparation-failure-test.c', EXP / 'code-memory.c')}}
    for entry in ('cli', 'gcd'):
        command = next(list(cmd) for cmd in report['commands'] if '-o' in cmd and Path(cmd[cmd.index('-o') + 1]).name == entry)
        includes = [command[i + 1] for i, flag in enumerate(command[:-1]) if flag == '-I']
        kernel = next(Path(path) / 'lkl-image.bin' for path in includes if (Path(path) / 'lkl-image.bin').is_file())
        original = next(path for path in command if path.endswith('/direct/code-memory.o'))
        command[command.index(original)] = str(renamed)
        command[command.index('-o') + 1] = str(out / entry)
        command.append(str(EXP / 'translation/preparation-failure-test.c'))
        subprocess.run(command, check=True, timeout=120)
        result['commands'].append(command)
        for attempt in range(1, 2 + 2 * config['cpus']):
            proc = subprocess.run([out / entry, kernel], env={**environment, 'NCE_TEST_PREPARATION_FAIL_AT': str(attempt)},
                                  capture_output=True, text=True, timeout=30)
            (out / f'{entry}-{attempt}.jsonl').write_text(proc.stdout)
            (out / f'{entry}-{attempt}.log').write_text(proc.stderr)
            rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith('{')]
            cleanup = [row for row in rows if row.get('test') == 'partial_preparation_cleanup']
            passed = (proc.returncode == 1 and cleanup == [{'test': 'partial_preparation_cleanup', 'passed': True,
                       'attempts': attempt, 'prepared': attempt - 1}])
            result['runs'].append({'entry': entry, 'failure_at': attempt, 'returncode': proc.returncode,
                                   'validated': passed, 'results': rows})
            (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            if not passed:
                raise SystemExit(f'准备失败处理未通过：{entry}-{attempt}')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
