#!/usr/bin/env python3
"""并发调用实际 init trace 头文件，检查全部 JSON 行、数量和客体字节。"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'experiments/native-linux/translation/stock-init-trace-test.c'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def canonical(row):
    return json.dumps(row, sort_keys=True, separators=(',', ':'))


def expected_rows(manifest, iterations):
    expected = Counter()
    payload = bytes((17 * i + 3) & 255 for i in range(2304))
    label = bytes((29 * i + 7) & 255 for i in range(256)).hex()
    attribute = 'security.selinux'.encode().hex()
    security_path = '/system/etc/selinux/trace-concurrency-file'.encode().hex()
    paths = [text.encode().hex() for text in ('/dev/loop-trace-concurrency', '/data/trace-concurrency', 'ext4')]
    for i in range(iterations):
        expected[canonical({'stock_init_write': 64, 'fd': 1000 + i, 'result': 2304, 'vector': 0,
                            'bytes': 2304, 'captured': 2048, 'hex': payload[:2048].hex()})] += 1
        for vector, offset, size in ((0, 37, 48), (1, 111, 96)):
            expected[canonical({'stock_init_write': 66, 'fd': 2000 + i, 'result': 144, 'vector': vector,
                                'bytes': size, 'captured': size, 'hex': payload[offset:offset + size].hex()})] += 1
        for number, fd, path, result in ((5, -1, security_path, 0), (7, 3000 + i, '', -13)):
            expected[canonical({'stock_init_security': number, 'result': result, 'fd': fd,
                                'path_hex': path, 'name_hex': attribute, 'value_hex': label})] += 1
        expected[canonical({'stock_init_block': 40, 'pid': manifest['pid'], 'result': -16 if i & 1 else 0,
                            'args': manifest['block_args'], 'paths_hex': paths})] += 1
        expected[canonical({'stock_init_syscall': 63, 'pc': 0x12340 + 4 * i,
                            'args': manifest['block_args'], 'result': 1})] += 1
    for i in range(iterations * 32):
        expected[canonical({'memory_sample': True, 'iteration': i,
                            'rss_bytes': 4096 * i + 1, 'guest_bytes': 16384 * i + 7})] += 1
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace-header', type=Path, default=ROOT / 'experiments/native-linux/translation/stock-init-trace.h')
    parser.add_argument('--iterations', type=int, default=128)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 4096:
        parser.error('--iterations 必须在 1..4096')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = {'test': 'stock_init_trace_concurrent_json', 'passed': False,
              'iterations': args.iterations, 'threads': 4, 'host': platform.platform()}
    try:
        header = args.trace_header.resolve(strict=True)
        inputs = {str(path): sha(path) for path in (SOURCE, Path(__file__).resolve(), header)}
        result['sha256'] = inputs
        compiler = shutil.which('clang')
        if not compiler:
            raise RuntimeError('缺少 clang')
        binary = out / 'stock-init-trace-test'
        build = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                 '-DSTOCK_INIT_TRACE_HEADER=' + json.dumps(str(header)), str(SOURCE), '-o', str(binary)]
        command = [str(binary), str(args.iterations)]
        result['commands'] = {'build': build, 'run': command}
        with (out / 'build.log').open('w') as log:
            subprocess.run(build, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        stdout_path, stderr_path = out / 'trace.jsonl', out / 'trace.stderr'
        with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=60)
        result['program_returncode'] = process.returncode
        lines = stdout_path.read_text().splitlines()
        parsed, parse_errors = [], []
        for number, line in enumerate(lines, 1):
            try:
                parsed.append(json.loads(line))
            except json.JSONDecodeError as error:
                parse_errors.append({'line': number, 'error': str(error),
                                     'line_sha256': hashlib.sha256(line.encode()).hexdigest(), 'preview': line[:512]})
        result['line_count'] = len(lines)
        result['expected_line_count'] = 1 + 39 * args.iterations
        result['parse_error_count'] = len(parse_errors)
        result['parse_errors'] = parse_errors[:8]
        result['artifacts'] = {path.name: {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)}
                               for path in (binary, stdout_path, stderr_path, out / 'build.log')}
        manifest = parsed[0] if parsed else {}
        if (not isinstance(manifest, dict) or manifest.get('stock_init_trace_fixture') is not True or
                type(manifest.get('iterations')) is not int or manifest['iterations'] != args.iterations or
                type(manifest.get('pid')) is not int or manifest['pid'] <= 0 or
                not isinstance(manifest.get('block_args'), list) or len(manifest['block_args']) != 6 or
                any(type(value) is not int for value in manifest['block_args'])):
            raise ValueError('fixture 首行无效')
        expected = expected_rows(manifest, args.iterations)
        actual = Counter(canonical(row) for row in parsed[1:])
        missing, unexpected = expected - actual, actual - expected
        result['missing_row_count'] = sum(missing.values())
        result['unexpected_row_count'] = sum(unexpected.values())
        result['missing_samples'] = [{'row': json.loads(row), 'count': count} for row, count in list(missing.items())[:3]]
        result['unexpected_samples'] = [{'row': json.loads(row), 'count': count} for row, count in list(unexpected.items())[:3]]
        calls = json.loads(stderr_path.read_text())
        result['stub_calls'] = calls
        result['inputs_unchanged'] = all(sha(Path(path)) == digest for path, digest in inputs.items())
        result['passed'] = (process.returncode == 0 and not parse_errors and not missing and not unexpected and
                            len(lines) == result['expected_line_count'] and result['inputs_unchanged'] and
                            calls.get('stock_init_trace_calls') is True and calls.get('threads') == 4 and
                            calls.get('getpid') == args.iterations and calls.get('copy_user', 0) > 0)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result['error'] = str(error)
    (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    if not result['passed']:
        (out / 'failure.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'report': str(out / 'result.json'),
                      'parse_error_count': result.get('parse_error_count')}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
