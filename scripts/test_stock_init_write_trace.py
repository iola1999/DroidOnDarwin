#!/usr/bin/env python3
"""编译实际 trace 头文件，检查默认与前缀筛选下的 write/writev 记录。"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
import probe_stock_init as probe

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'experiments/native-linux/translation/stock-init-trace.h'
TEXT = b'<6>fixture\n'
BINARY = b'\x7fELF\x00\x80'
PRINTABLE = b'plain\n'
PREFIXED_BINARY = b'<\x00\xff'
LARGE_TEXT = b'<' + b'X' * 2047
LARGE_BINARY = b'Z' * 2048
FILE_LIMIT = 64 << 10
CASES = (
    ('text', 64, 1, 11, ((0, 11, TEXT, True),), (1, 11, 0)),
    ('binary', 64, 9, 6, ((0, 6, BINARY, False),), (1, 6, 0)),
    ('printable_without_prefix', 64, 2, 6, ((0, 6, PRINTABLE, False),), (1, 6, 0)),
    ('prefixed_binary', 64, 9, 3, ((0, 3, PREFIXED_BINARY, True),), (1, 3, 0)),
    ('text_exact_limit', 64, 1, 2048, ((0, 2048, LARGE_TEXT, True),), (1, 2048, 0)),
    ('text_truncated', 64, 1, 4096, ((0, 4096, LARGE_TEXT, True),), (1, 2048, 0)),
    ('binary_truncated', 64, 9, 4096, ((0, 4096, LARGE_BINARY, False),), (1, 2048, 0)),
    ('negative_result', 64, 1, -5, ((0, 11, TEXT, True),), (1, 11, 0)),
    ('writev_mixed', 66, 4, 26, ((0, 11, TEXT, True), (1, 6, BINARY, False),
                              (2, 3, PREFIXED_BINARY, True), (4, 6, PRINTABLE, False)), (9, 106, 0)),
    ('writev_data_copy_failure', 66, 1, -14, ((0, 11, TEXT, True), (2, 11, TEXT, True)), (6, 78, 1)),
    ('writev_descriptor_copy_failure', 66, 1, -14, ((0, 11, TEXT, True),), (3, 43, 1)),
    ('data_copy_failure', 64, 1, -14, (), (1, 11, 1)),
    ('zero_length', 64, 1, 0, (), (0, 0, 0)),
    ('writev_empty', 66, 1, 0, (), (2, 32, 0)),
    ('writev_eight_vector_limit', 66, 1, 99, tuple((i, 11, TEXT, True) for i in range(8)), (16, 216, 0)),
    ('other_syscall', 63, 1, 1, (), (0, 0, 0)),
)
FIXTURE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static unsigned long copy_calls, copy_bytes, copy_failures, failed_address;

static int copy_user(void *destination, unsigned long address, unsigned long bytes, int write)
{
    assert(write == 0);
    ++copy_calls;
    copy_bytes += bytes;
    if (failed_address && address == failed_address) {
        ++copy_failures;
        memset(destination, '<', bytes);
        return -14;
    }
    assert(address != 1);
    memcpy(destination, (const void *)(uintptr_t)address, bytes);
    return 0;
}

static long run_syscall(long number, const long *args)
{
    (void)number;
    (void)args;
    abort();
}

#include STOCK_INIT_TRACE_HEADER

static void trace_case(const char *name, long number, long fd, unsigned long address,
                       long length, long result, unsigned long failure)
{
    copy_calls = copy_bytes = copy_failures = 0;
    failed_address = failure;
    printf("{\"fixture_case\":\"%s\"}\n", name);
    long args[6] = {fd, (long)address, length, 0, 0, 0};
    stock_init_trace_write(number, args, result);
    printf("{\"fixture_copies\":true,\"calls\":%lu,\"bytes\":%lu,\"failed\":%lu}\n",
           copy_calls, copy_bytes, copy_failures);
}

int main(void)
{
    _Static_assert(sizeof(long) == 8, "fixture requires 64-bit long");
    struct rlimit files = {65536, 65536}, cores = {0, 0};
    assert(setrlimit(RLIMIT_FSIZE, &files) == 0);
    assert(setrlimit(RLIMIT_CORE, &cores) == 0);
    (void)stock_init_trace_security;
    (void)stock_init_trace_result;
    (void)stock_init_trace_summary;
    (void)stock_init_trace_block;
    unsigned char text[] = "<6>fixture\n", binary[] = {0x7f, 'E', 'L', 'F', 0, 0x80};
    unsigned char printable[] = "plain\n", prefixed_binary[] = {'<', 0, 0xff};
    unsigned char large_text[4096], large_binary[4096];
    memset(large_text, 'X', sizeof(large_text));
    memset(large_binary, 'Z', sizeof(large_binary));
    large_text[0] = '<';
    unsigned long mixed[][2] = {{(unsigned long)text, 11}, {(unsigned long)binary, 6},
                               {(unsigned long)prefixed_binary, 3}, {1, 0}, {(unsigned long)printable, 6}};
    unsigned long data_failure[][2] = {{(unsigned long)text, 11}, {1, 8}, {(unsigned long)text, 11}};
    unsigned long empty[][2] = {{1, 0}, {1, 0}};
    unsigned long nine[9][2];
    for (unsigned i = 0; i < 9; ++i) {
        nine[i][0] = i < 8 ? (unsigned long)text : 1;
        nine[i][1] = 11;
    }
    trace_case("text", 64, 1, (unsigned long)text, 11, 11, 0);
    trace_case("binary", 64, 9, (unsigned long)binary, 6, 6, 0);
    trace_case("printable_without_prefix", 64, 2, (unsigned long)printable, 6, 6, 0);
    trace_case("prefixed_binary", 64, 9, (unsigned long)prefixed_binary, 3, 3, 0);
    trace_case("text_exact_limit", 64, 1, (unsigned long)large_text, 2048, 2048, 0);
    trace_case("text_truncated", 64, 1, (unsigned long)large_text, 4096, 4096, 0);
    trace_case("binary_truncated", 64, 9, (unsigned long)large_binary, 4096, 4096, 0);
    trace_case("negative_result", 64, 1, (unsigned long)text, 11, -5, 0);
    trace_case("writev_mixed", 66, 4, (unsigned long)mixed, 5, 26, 0);
    trace_case("writev_data_copy_failure", 66, 1, (unsigned long)data_failure, 3, -14, 1);
    trace_case("writev_descriptor_copy_failure", 66, 1, (unsigned long)data_failure, 3, -14, (unsigned long)&data_failure[1]);
    trace_case("data_copy_failure", 64, 1, 1, 11, -14, 1);
    trace_case("zero_length", 64, 1, 1, 0, 0, 0);
    trace_case("writev_empty", 66, 1, (unsigned long)empty, 2, 0, 0);
    trace_case("writev_eight_vector_limit", 66, 1, (unsigned long)nine, 9, 99, 0);
    trace_case("other_syscall", 63, 1, 1, 1, 1, 0);
    return fflush(stdout) == 0 ? 0 : 1;
}
'''


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_rows(rows, text_only):
    checks, position = [], 0
    for name, number, fd, returned, vectors, copies in CASES:
        if position >= len(rows) or rows[position] != {'fixture_case': name}:
            raise ValueError('fixture 调用顺序变化：' + name)
        position += 1
        actual = []
        while position < len(rows) and 'stock_init_write' in rows[position]:
            actual.append(rows[position])
            position += 1
        expected = [{'stock_init_write': number, 'fd': fd, 'result': returned, 'vector': index,
                     'bytes': length, 'captured': len(data), 'hex': data.hex()}
                    for index, length, data, keep in vectors if keep or not text_only]
        expected_copies = dict(zip(('calls', 'bytes', 'failed'), copies), fixture_copies=True)
        copied = rows[position] if position < len(rows) else None
        position += 1
        passed = actual == expected and copied == expected_copies
        check = {'name': name, 'passed': passed, 'recorded': len(actual),
                 'expected_recorded': len(expected), 'copies': copied, 'expected_copies': expected_copies}
        if not passed:
            check.update(observed_rows=actual, expected_rows=expected)
        checks.append(check)
    if position != len(rows):
        raise ValueError('fixture 存在额外记录')
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    common = Path(subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    output = args.output.resolve()
    if not output.is_relative_to(common.resolve().parent / 'work/native-linux-nce'):
        parser.error('输出需要位于 MAIN/work/native-linux-nce')
    output.mkdir(parents=True, exist_ok=False)
    sources = (Path(__file__).resolve(), HEADER, Path(probe.__file__).resolve())
    before = {str(path): sha(path) for path in sources}
    result = {'test': 'stock_init_write_trace', 'passed': False, 'host': platform.platform(),
              'source_sha256_before': before, 'fixtures': [],
              'scope': '宿主 C 调用实际 header；默认与 TEXT_ONLY 的复制、记录边界及生产 guest_write_text 表达式。'}
    try:
        assignments = [node for node in ast.walk(ast.parse(Path(probe.__file__).read_text()))
                       if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call) and
                       any(isinstance(target, ast.Name) and target.id == 'guest_writes' for target in node.targets)]
        if len(assignments) != 1:
            raise ValueError('guest_write_text 的生产表达式需要重新核对')
        consumer = compile(ast.Expression(assignments[0].value), str(probe.__file__), 'eval')
        result['guest_write_text_source_line'] = assignments[0].lineno
        expected_text = ''.join(data.decode('utf-8', errors='replace')
                                for _, _, _, _, vectors, _ in CASES for _, _, data, keep in vectors if keep)
        compiler = shutil.which('clang')
        if not compiler:
            raise RuntimeError('缺少 clang')
        result['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True, timeout=10).splitlines()[0]
        source = output / 'write-fixture.c'
        source.write_text(FIXTURE)
        for mode, text_only in (('default', False), ('text_only', True)):
            binary = output / mode
            build = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                     '-DSTOCK_INIT_TRACE_HEADER=' + json.dumps(str(HEADER)),
                     *(['-DNCE_STOCK_INIT_LOG_TEXT_ONLY'] if text_only else []), str(source), '-o', str(binary)]
            fixture = {'profile': mode, 'build_command': build, 'command': [str(binary)], 'checks': []}
            result['fixtures'].append(fixture)
            with (output / (mode + '.build.log')).open('w') as log:
                subprocess.run(build, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
            stdout_path, stderr_path = output / (mode + '.jsonl'), output / (mode + '.stderr')
            with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
                subprocess.run([str(binary)], stdout=stdout, stderr=stderr, check=True, timeout=10)
            if any(path.stat().st_size > FILE_LIMIT for path in (stdout_path, stderr_path)):
                raise ValueError('fixture 输出超过 64 KiB')
            rows = probe.read_observations(stdout_path)
            fixture['checks'] = check_rows(rows, text_only)
            guest_text = eval(consumer, {'rows': rows})
            fixture['checks'].append({'name': 'production_guest_write_text', 'passed': guest_text == expected_text,
                                      'characters': len(guest_text), 'expected_characters': len(expected_text)})
            fixture['passed'] = all(check['passed'] for check in fixture['checks'])
            fixture['stdout_bytes'] = stdout_path.stat().st_size
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result['error'] = str(error)
    result['source_sha256_after'] = {path: sha(Path(path)) for path in before}
    result['source_unchanged'] = before == result['source_sha256_after']
    result['passed'] = ('error' not in result and result['source_unchanged'] and len(result['fixtures']) == 2 and
                        all(fixture.get('passed') for fixture in result['fixtures']))
    result['artifacts'] = {path.name: {'bytes': path.stat().st_size, 'sha256': sha(path)}
                           for path in sorted(output.iterdir()) if path.is_file()}
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': sum(len(fixture['checks']) for fixture in result['fixtures']),
                      'report': str(output / 'result.json')}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
