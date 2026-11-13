#!/usr/bin/env python3
"""调用实际 trace 头文件，检查失败 msync 的采样、汇总与历史格式。"""
import argparse
from copy import deepcopy
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
PID = 4242
PC = 0x100001000
ARGS = [0x10000000, 16384, 0, 17, 18, 19]
LAST_PC = 0x100002000
LAST_ARGS = [0x20000000, 32768, 0, 33, 34, 35]
BLOCK_ARGS = [7, 123, 0, 0, 0, 0]
GUEST = {'guest_base': 1 << 32, 'guest_end': 1 << 39}
FILE_LIMIT = 64 << 10
FIXTURE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static unsigned long copy_calls, getpid_calls, failed_calls, successful_calls;

static int copy_user(void *destination, unsigned long address, unsigned long bytes, int write)
{
    (void)destination;
    (void)address;
    (void)bytes;
    (void)write;
    ++copy_calls;
    abort();
}

static long run_syscall(long number, const long *args)
{
    assert(number == 172);
    for (unsigned i = 0; i < 6; ++i)
        assert(args[i] == 0);
    ++getpid_calls;
    return 4242;
}

#include STOCK_INIT_TRACE_HEADER

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    char *end;
    unsigned long count = strtoul(argv[1], &end, 10);
    if (*end || (count != 1000000 && count != 1048576))
        return 2;
    assert(sizeof(long) == 8);
    struct rlimit files = {65536, 65536}, cores = {0, 0};
    assert(setrlimit(RLIMIT_FSIZE, &files) == 0);
    assert(setrlimit(RLIMIT_CORE, &cores) == 0);
    (void)stock_init_trace_write;
    (void)stock_init_trace_security;

    long block[6] = {7, 123, 0, 0, 0, 0};
    stock_init_trace_block(29, block, -22);
    long args[6] = {0x10000000, 16384, 0, 17, 18, 19};
    long last_args[6] = {0x20000000, 32768, 0, 33, 34, 35};
    long success_args[6] = {0x30000000, 4096, 0, 49, 50, 51};
    for (unsigned i = 0; i < 17; ++i) {
        stock_init_trace_result(227, success_args, 0, 0x100003000UL);
        ++successful_calls;
    }
    for (unsigned long i = 1; i <= count; ++i) {
        stock_init_trace_result(227, i == count ? last_args : args,
                                i == count ? -22 : -12,
                                i == count ? 0x100002000UL : 0x100001000UL);
        ++failed_calls;
    }
    for (unsigned i = 0; i < 23; ++i) {
        stock_init_trace_result(227, success_args, 0, 0x100003000UL);
        ++successful_calls;
    }
    stock_init_trace_summary();
    assert(copy_calls == 0 && getpid_calls == 1);
    assert(failed_calls == count && successful_calls == 40);
    assert(fflush(stdout) == 0);
    assert(fprintf(stderr,
                   "{\"fixture_calls\":true,\"copy_user\":%lu,\"getpid\":%lu,\"failed_msync\":%lu,\"successful_msync\":%lu}\n",
                   copy_calls, getpid_calls, failed_calls, successful_calls) > 0);
    return 0;
}
'''


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def expected_rows(count):
    indices = list(range(1, 65))
    index = 128
    while index <= count:
        indices.append(index)
        index *= 2
    rows = [{'stock_init_block': 29, 'pid': PID, 'result': -22,
             'args': BLOCK_ARGS, 'paths_hex': []}]
    rows += [{'stock_init_syscall': 227, 'trace_index': index,
              'pc': LAST_PC if index == count else PC,
              'args': LAST_ARGS if index == count else ARGS,
              'result': -22 if index == count else -12} for index in indices]
    rows.append({'stock_init_msync_summary': True, 'seen': count, 'recorded': len(indices),
                 'omitted': count - len(indices), 'last_pc': LAST_PC,
                 'last_args': LAST_ARGS, 'last_result': -22})
    return rows


def validator_cases(rows):
    checks = []

    def check(name, candidate, expected):
        actual = probe._validate_msync_trace(candidate, GUEST)
        checks.append({'name': name, 'expected': expected, 'actual': actual, 'passed': actual is expected})

    check('actual_power_of_two_end', rows, True)
    check('no_failed_msync', [], True)
    legacy = deepcopy(rows[1])
    legacy.pop('trace_index')
    check('historical_unindexed_failure', [legacy], True)
    check('indexed_failure_missing_summary', rows[:-1], False)
    check('summary_missing_samples', [rows[-1]], False)
    check('duplicate_summary', rows + [deepcopy(rows[-1])], False)
    check('summary_before_last_sample', [rows[0], rows[-1], *rows[1:-1]], False)

    for field, value in (('recorded', rows[-1]['recorded'] + 1),
                         ('omitted', rows[-1]['omitted'] + 1),
                         ('last_pc', LAST_PC + 4),
                         ('last_args', [LAST_ARGS[0] + 16384, *LAST_ARGS[1:]]),
                         ('last_result', -12),
                         ('stock_init_msync_summary', False),
                         ('last_pc', GUEST['guest_end']),
                         ('last_args', LAST_ARGS[:-1]),
                         ('last_result', 0)):
        changed = deepcopy(rows)
        changed[-1][field] = value
        check('wrong_summary_' + field + '_' + str(len(checks)), changed, False)

    changed = deepcopy(rows)
    changed[-1]['seen'] -= 1
    changed[-1]['omitted'] = changed[-1]['seen'] - changed[-1]['recorded']
    check('wrong_seen_with_consistent_omitted', changed, False)

    changed = deepcopy(rows)
    changed[1]['trace_index'] = 2
    check('wrong_first_index', changed, False)
    changed = deepcopy(rows)
    changed[1], changed[2] = changed[2], changed[1]
    check('out_of_order_indices', changed, False)
    changed = deepcopy(rows)
    del changed[1]
    changed[-1]['recorded'] -= 1
    changed[-1]['omitted'] += 1
    check('missing_index_with_consistent_counts', changed, False)
    changed = deepcopy(rows)
    extra = deepcopy(changed[64])
    extra['trace_index'] = 65
    changed.insert(65, extra)
    changed[-1]['recorded'] += 1
    changed[-1]['omitted'] -= 1
    check('extra_non_power_index_65', changed, False)
    changed = deepcopy(rows)
    changed.insert(-1, legacy)
    check('new_format_mixed_unindexed_failure', changed, False)
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
    inputs = {str(path): sha(path) for path in sources}
    result = {'test': 'stock_init_msync_trace', 'passed': False, 'host': platform.platform(),
              'source_sha256': inputs, 'fixtures': [], 'checks': [],
              'scope': '宿主 C 直接调用实际 trace 头文件；失败 msync 的有界记录、末次参数与汇总校验。copy_user 调用即终止，运行不读取客体内存。'}
    try:
        compiler = shutil.which('clang')
        if not compiler:
            raise RuntimeError('缺少 clang')
        result['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True, timeout=10).splitlines()[0]
        source = output / 'msync-fixture.c'
        source.write_text(FIXTURE)
        binary = output / 'msync-fixture'
        build = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-DSTOCK_INIT_TRACE_HEADER=' + json.dumps(str(HEADER)), str(source), '-o', str(binary)]
        result['build_command'] = build
        with (output / 'build.log').open('w') as log:
            subprocess.run(build, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        for count in (1000000, 1 << 20):
            stdout_path, stderr_path = output / f'{count}.jsonl', output / f'{count}.stderr'
            command = [str(binary), str(count)]
            with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
                process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=30)
            if process.returncode != 0:
                raise ValueError(f'fixture 退出码 {process.returncode}：{count}')
            actual = probe.read_observations(stdout_path)
            expected = expected_rows(count)
            calls = json.loads(probe._read_log_text(stderr_path))
            summary = actual[-1] if actual else None
            passed = (actual == expected and len(actual) <= 128 and
                      stdout_path.stat().st_size <= FILE_LIMIT and stderr_path.stat().st_size <= FILE_LIMIT and
                      calls == {'fixture_calls': True, 'copy_user': 0, 'getpid': 1,
                                'failed_msync': count, 'successful_msync': 40} and
                      probe._validate_msync_trace(actual, GUEST) is True)
            result['fixtures'].append({'failures': count, 'command': command, 'passed': passed,
                                       'line_count': len(actual), 'expected_line_count': len(expected),
                                       'summary': summary, 'calls': calls,
                                       'last_failure_recorded': count == 1 << 20})
            if not passed:
                raise ValueError(f'实际头文件的记录与固定调用不一致：{count}')
            if count == 1 << 20:
                result['checks'] = validator_cases(actual)
        result['source_unchanged'] = all(sha(Path(path)) == value for path, value in inputs.items())
        result['passed'] = (result['source_unchanged'] and all(row['passed'] for row in result['fixtures']) and
                            all(row['passed'] for row in result['checks']))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result['error'] = str(error)
    result['artifacts'] = {path.name: {'bytes': path.stat().st_size, 'sha256': sha(path)}
                           for path in sorted(output.iterdir()) if path.is_file()}
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'fixtures': len(result['fixtures']),
                      'checks': len(result['checks']), 'report': str(output / 'result.json')}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
