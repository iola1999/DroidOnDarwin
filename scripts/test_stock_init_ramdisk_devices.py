#!/usr/bin/env python3
"""编译真实 ramdisk 设备部署函数的宿主回归，Linux syscall 由 C fixture 模拟。"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ramdisk-header', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    header = args.ramdisk_header.resolve(strict=True)
    if header.name != 'sdk-ramdisk.h':
        parser.error('--ramdisk-header 必须指向已生成的 sdk-ramdisk.h')
    output = args.output.resolve()
    if (output / 'result.json').exists():
        parser.error('输出已有 result.json；新运行使用独立目录')
    output.mkdir(parents=True, exist_ok=True)
    fixture = ROOT / 'experiments/native-linux/translation/stock-init-ramdisk-devices-test.c'
    production = fixture.with_name('stock-init-ramdisk.h')
    paths = [Path(__file__).resolve(), fixture, production, header]
    before = {str(path): digest(path) for path in paths}
    executable = output / 'stock-init-ramdisk-devices-host-test'
    command = ['xcrun', 'clang', '-std=c11', '-O0', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-iquote', str(header.parent), str(fixture), '-o', str(executable)]
    result = {'passed': False, 'scope': '真实 C device/attributes/mtime/verify；Linux syscall 为宿主模拟，完整29项与客体行为由raw验收。',
              'host': {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine()},
              'inputs': before, 'commands': [command], 'cases': [], 'checks': [], 'cleanup': []}
    try:
        compiled = subprocess.run(command, text=True, capture_output=True, timeout=60)
        (output / 'compile.stdout').write_text(compiled.stdout)
        (output / 'compile.stderr').write_text(compiled.stderr)
        result['compile_returncode'] = compiled.returncode
        if compiled.returncode:
            raise RuntimeError('宿主 C fixture 编译失败')
        result['executable_sha256'] = digest(executable)
        result['commands'].append([str(executable)])
        observed = subprocess.run([str(executable)], text=True, capture_output=True, timeout=10)
        (output / 'stdout.jsonl').write_text(observed.stdout)
        (output / 'stderr.log').write_text(observed.stderr)
        result['returncode'] = observed.returncode
        rows = [json.loads(line) for line in observed.stdout.splitlines() if line.strip()]
        cases = [row for row in rows if 'stock_init_ramdisk_device_case' in row]
        summaries = [row for row in rows if row.get('stock_init_ramdisk_devices_host_test') is True]
        result['cases'] = cases
        result['checks'] = [
            {'name': 'host-fixture-exit', 'passed': observed.returncode == 0},
            {'name': 'all-device-cases', 'passed': bool(cases) and all(row.get('passed') is True for row in cases)},
            {'name': 'fixture-summary', 'passed': len(summaries) == 1 and summaries[0].get('passed') is True and
             summaries[0].get('cases') == summaries[0].get('passed_cases') == len(cases)},
        ]
    except Exception as error:
        result['failure'] = {'type': type(error).__name__, 'message': str(error)}
        if isinstance(error, subprocess.TimeoutExpired):
            result['failure']['stdout'] = (error.stdout or b'').decode(errors='replace') if isinstance(error.stdout, bytes) else error.stdout
            result['failure']['stderr'] = (error.stderr or b'').decode(errors='replace') if isinstance(error.stderr, bytes) else error.stderr
    finally:
        result['checks'].append({'name': 'inputs-unchanged', 'passed': before == {str(path): digest(path) for path in paths}})
        if executable.exists():
            cleanup = {'path': str(executable), 'bytes': executable.stat().st_size, 'sha256': digest(executable)}
            executable.unlink()
            cleanup['removed'] = not executable.exists()
            result['cleanup'].append(cleanup)
        result['passed'] = 'failure' not in result and bool(result['cases']) and all(check['passed'] for check in result['checks'])
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'cases': len(result['cases']),
                      'result': str(output / 'result.json'), 'sha256': digest(output / 'result.json')}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
