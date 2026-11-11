#!/usr/bin/env python3
"""逐次拒绝执行内存准备，检查 CLI/GCD 的执行内存和完整宿主预留区释放。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
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
    direct = config['execution'] == 'direct'
    unicorn_4k = config['execution'] == 'unicorn' and config.get('guest_page_bytes') == 4096
    if config['code_memory'] != 'dual' or config['workload'] != 'core' or not (direct or unicorn_4k):
        parser.error('需要双映射 Direct 或受限 4 KiB Unicorn 核心检查结果')
    if not all(run['validated'] for run in report['runs']):
        parser.error('核心检查尚未成功')
    layout = config.get('guest_layout', 'host')
    if layout not in ('host', 'low', 'sparse') or (layout in ('low', 'sparse') and (
            config['mapping'] != 'pte' or config['cpus'] != 1 or (direct and config['direct']['memory'] != 'flat'))):
        parser.error('客体布局与核心检查配置不兼容')
    for name, digest in report['sha256'].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            parser.error('输入已变化：' + name)
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'work/native-linux-nce'):
        parser.error('输出必须放在 work/native-linux-nce')
    out.mkdir(parents=True, exist_ok=True)
    renamed = out / 'code-memory.o'
    prepare_command = ['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-Dnce_code_prepare_range=real_prepare_range', '-Dnce_code_release_range=real_release_range',
                       '-c', str(EXP / 'code-memory.c'), '-o', str(renamed)]
    subprocess.run(prepare_command, check=True, timeout=60)
    execution = out / 'linux-execution.c'
    execution.write_text('#define mmap nce_test_reservation_mmap\n'
                         '#define munmap nce_test_reservation_munmap\n'
                         '#define mach_vm_map nce_test_reservation_mach_vm_map\n'
                         '#include ' + json.dumps(str(EXP / 'translation/linux-execution.c')) + '\n')
    environment = {**{key: value for key, value in os.environ.items() if not key.startswith('NCE_')}, 'NCE_TRANSLATION_CACHE_MIB': '16', 'NCE_TRANSLATION_PREEMPTION': 'timer',
                   'NCE_TRANSLATION_GUEST_LAYOUT': layout, 'NCE_TRANSLATION_DATA_TBI': str(int(config.get('data_tbi', False)))}
    if direct:
        environment.update(NCE_DIRECT_MEMORY='flat', NCE_DIRECT_NATIVE_EXCLUSIVE='1', NCE_DIRECT_CROSS_PAGE='1')
    result = {'source_report': str(args.report), 'source_report_sha256': hashlib.sha256(args.report.read_bytes()).hexdigest(),
              'platform': platform.platform(),
              'cpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
              'system_build': subprocess.check_output(['sw_vers', '-buildVersion'], text=True).strip(),
              'source_configuration': config, 'test_environment': {key: value for key, value in environment.items() if key.startswith('NCE_')},
              'commands': [prepare_command], 'runs': [], 'sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in (Path(__file__).resolve(), EXP / 'translation/preparation-failure-test.c',
                               EXP / 'code-memory.c', EXP / 'layout.h', execution, renamed)}}
    for entry in ('cli', 'gcd'):
        command = next(list(cmd) for cmd in report['commands'] if '-o' in cmd and Path(cmd[cmd.index('-o') + 1]).name == entry)
        includes = [command[i + 1] for i, flag in enumerate(command[:-1]) if flag == '-I']
        kernel = next(Path(path) / 'lkl-image.bin' for path in includes if (Path(path) / 'lkl-image.bin').is_file())
        original = next(path for path in command if path.endswith('/direct/code-memory.o')) if direct else str(EXP / 'code-memory.c')
        command[command.index(original)] = str(renamed)
        command[command.index(str(EXP / 'translation/linux-execution.c'))] = str(execution)
        command[command.index('-o') + 1] = str(out / entry)
        command.append(str(EXP / 'translation/preparation-failure-test.c'))
        subprocess.run(command, check=True, timeout=120)
        result['commands'].append(command)
        result['sha256'][str((out / entry).relative_to(ROOT))] = hashlib.sha256((out / entry).read_bytes()).hexdigest()
        last_attempt = 1 + (2 if direct else 1) * config['cpus']
        cases = [(attempt, False) for attempt in range(1, last_attempt + 1)] + [(last_attempt, True)]
        for attempt, retain_tail in cases:
            case = 'retained-tail' if retain_tail else 'complete'
            proc = subprocess.run([out / entry, kernel], env={**environment, 'NCE_TEST_PREPARATION_FAIL_AT': str(attempt),
                                  'NCE_TEST_RESERVATION_RETAIN_TAIL': str(int(retain_tail))},
                                  capture_output=True, text=True, timeout=30)
            (out / f'{entry}-{attempt}-{case}.jsonl').write_text(proc.stdout)
            (out / f'{entry}-{attempt}-{case}.log').write_text(proc.stderr)
            rows = [json.loads(line) for line in proc.stdout.splitlines() if line.startswith('{')]
            cleanup = [row for row in rows if row.get('test') == 'partial_preparation_cleanup']
            reservations = [row for row in rows if row.get('test') == 'partial_reservation_cleanup']
            created = attempt > 1
            address = reservations[0].get('address') if len(reservations) == 1 else None
            size = (1 << 30) + (64 << 20) + (1 << 32 if layout == 'host' else 0)
            valid_address = type(address) is int and (address >= 1 << (36 if layout == 'sparse' else 32) and address % 16384 == 0 if created else address == 0)
            expected_reservation = {'test': 'partial_reservation_cleanup', 'passed': not retain_tail,
                                    'layout': layout, 'created': created, 'address': address,
                                    'bytes': size if created else 0, 'releases': int(created),
                                    'unmapped': created and not retain_tail}
            passed = (proc.returncode == 1 and cleanup == [{'test': 'partial_preparation_cleanup', 'passed': True,
                       'attempts': attempt, 'prepared': attempt - 1}] and valid_address and reservations == [expected_reservation])
            result['runs'].append({'entry': entry, 'failure_at': attempt, 'case': case, 'returncode': proc.returncode,
                                   'validated': passed, 'results': rows})
            (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            if not passed:
                raise SystemExit(f'准备失败处理未通过：{entry}-{attempt}-{case}')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
