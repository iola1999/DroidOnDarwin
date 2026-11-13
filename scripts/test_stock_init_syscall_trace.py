#!/usr/bin/env python3
"""重复调用实际 trace 头文件，检查 read 与失败调用的采样和末次汇总。"""
import argparse
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
import probe_stock_init as probe

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'experiments/native-linux/translation/stock-init-trace.h'
GUEST = {'guest_base': 1 << 32, 'guest_end': 1 << 39}
PC, LAST_PC = 0x100001000, 0x100002000
ARGS, LAST_ARGS = [0x10000000, 4096, 0, 17, 18, 19], [0x20000000, 1024, 0, 33, 34, 35]
FIXTURE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static int copy_user(void *destination, unsigned long address, unsigned long bytes, int write)
{
    (void)destination; (void)address; (void)bytes; (void)write;
    abort();
}
static long run_syscall(long number, const long *args)
{
    (void)number; (void)args;
    abort();
}
#include STOCK_INIT_TRACE_HEADER

int main(int argc, char **argv)
{
    assert(argc == 2);
    char *end;
    unsigned long count = strtoul(argv[1], &end, 10);
    assert(!*end && (count == 1000000 || count == 1048576));
    struct rlimit files = {65536, 65536}, cores = {0, 0};
    assert(!setrlimit(RLIMIT_FSIZE, &files) && !setrlimit(RLIMIT_CORE, &cores));
    (void)stock_init_trace_write;
    (void)stock_init_trace_security;
    (void)stock_init_trace_block;
    long args[6] = {0x10000000, 4096, 0, 17, 18, 19};
    long last[6] = {0x20000000, 1024, 0, 33, 34, 35};
    for (unsigned long i = 1; i <= count; ++i) {
        const long *values = i == count ? last : args;
        unsigned long pc = i == count ? 0x100002000UL : 0x100001000UL;
        stock_init_trace_result(63, values, i == count ? 1024 : 4096, pc);
        stock_init_trace_result(63, values, -9, pc);
        stock_init_trace_result(167, values, -22, pc);
    }
    for (unsigned i = 0; i < 17; ++i) {
        stock_init_trace_result(999, args, -38, 0x100001000UL);
        stock_init_trace_result(-1, last, -38, 0x100002000UL);
        stock_init_trace_result(64, args, 0, 0x100001000UL);
        stock_init_trace_result(227, args, 0, 0x100001000UL);
    }
    stock_init_trace_summary();
    assert(!fflush(stdout));
    fprintf(stderr, "{\"fixture_calls\":true,\"read_success\":%lu,\"read_failure\":%lu,"
                    "\"prctl_failure\":%lu,\"unknown_failure\":34,\"ignored_success\":34}\n",
            count, count, count);
    return 0;
}
'''


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def expected_rows(count):
    indices = [*range(1, 65), *(1 << bit for bit in range(7, count.bit_length()))]
    rows = []
    for index in indices:
        for number, result in ((63, 1024 if index == count else 4096), (63, -9), (167, -22)):
            rows.append({'stock_init_syscall': number, 'trace_index': index,
                         'pc': LAST_PC if index == count else PC,
                         'args': LAST_ARGS if index == count else ARGS, 'result': result})
    for i in range(17):
        for number, offset, values, pc in ((999, 1, ARGS, PC), (-1, 2, LAST_ARGS, LAST_PC)):
            rows.append({'stock_init_syscall': number, 'trace_index': 2 * i + offset,
                         'pc': pc, 'args': values, 'result': -38})
    for number, failed, result in ((63, False, 1024), (63, True, -9), (167, True, -22), (-1, True, -38)):
        seen, recorded = (34, 34) if number == -1 else (count, len(indices))
        rows.append({'stock_init_syscall_summary': True, 'syscall': number, 'failed': failed,
                     'last_syscall': number, 'seen': seen, 'recorded': recorded, 'omitted': seen - recorded,
                     'last_pc': LAST_PC, 'last_args': LAST_ARGS, 'last_result': result})
    return rows


def validator_cases(rows):
    checks = []

    def check(name, candidate, expected):
        actual = probe._validate_syscall_trace(candidate, GUEST)
        checks.append({'name': name, 'expected': expected, 'actual': actual, 'passed': actual is expected})

    check('actual', rows, True)
    check('historical_without_summary', [{k: v for k, v in rows[0].items() if k != 'trace_index'}], True)
    check('missing_summary', rows[:-1], False)
    check('summary_without_samples', rows[-4:], False)
    check('duplicate_summary', [*rows, rows[-1]], False)
    check('summary_before_samples', [*rows[-4:], *rows[:-4]], False)
    for field, value in (('seen', rows[-4]['seen'] - 1), ('recorded', 0), ('omitted', 0),
                         ('failed', True), ('last_syscall', 56), ('last_pc', GUEST['guest_end']),
                         ('last_args', []), ('last_result', -22), ('stock_init_syscall_summary', False)):
        changed = deepcopy(rows)
        changed[-4][field] = value
        check('invalid_summary_' + field, changed, False)
    changed = deepcopy(rows)
    changed[0]['trace_index'] = 2
    check('wrong_first_index', changed, False)
    changed = deepcopy(rows)
    changed[0], changed[3] = changed[3], changed[0]
    check('sample_order', changed, False)
    changed = deepcopy(rows)
    changed.insert(1, {k: v for k, v in rows[0].items() if k != 'trace_index'})
    check('mixed_historical_sample', changed, False)
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    inputs = {str(path): sha(path) for path in (Path(__file__).resolve(), HEADER, Path(probe.__file__).resolve())}
    report = {'test': 'stock_init_syscall_trace', 'passed': False, 'sha256': inputs, 'fixtures': [], 'checks': []}
    try:
        source, binary = out / 'fixture.c', out / 'fixture'
        source.write_text(FIXTURE)
        build = [shutil.which('clang'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 '-DSTOCK_INIT_TRACE_HEADER=' + json.dumps(str(HEADER)), str(source), '-o', str(binary)]
        report['build_command'] = build
        with (out / 'build.log').open('w') as log:
            subprocess.run(build, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        for count in (1000000, 1 << 20):
            stdout_path, stderr_path = out / f'{count}.jsonl', out / f'{count}.stderr'
            with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
                result = subprocess.run([str(binary), str(count)], stdout=stdout, stderr=stderr, timeout=30)
            actual = probe.read_observations(stdout_path)
            expected = expected_rows(count)
            calls = json.loads(probe._read_log_text(stderr_path))
            passed = (result.returncode == 0 and actual == expected and stdout_path.stat().st_size <= 65536 and
                      calls == {'fixture_calls': True, 'read_success': count, 'read_failure': count,
                                'prctl_failure': count, 'unknown_failure': 34, 'ignored_success': 34} and
                      probe._validate_syscall_trace(actual, GUEST) and probe._validate_msync_trace(actual, GUEST))
            report['fixtures'].append({'count_per_class': count, 'passed': passed, 'returncode': result.returncode,
                                       'rows': len(actual), 'bytes': stdout_path.stat().st_size, 'calls': calls,
                                       'summaries': actual[-4:]})
            assert passed, '实际通用调用采样与固定调用不一致'
            if count == 1 << 20:
                report['checks'] = validator_cases(actual)
        report['inputs_unchanged'] = all(sha(path) == value for path, value in inputs.items())
        report['passed'] = report['inputs_unchanged'] and all(x['passed'] for x in report['checks'])
    except Exception as error:
        report.update(error=str(error), error_type=type(error).__name__)
    report['artifacts'] = {p.name: {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in out.iterdir() if p.is_file()}
    (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'fixtures': len(report['fixtures']), 'checks': len(report['checks']),
                      'report': str(out / 'result.json')}, ensure_ascii=False))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
