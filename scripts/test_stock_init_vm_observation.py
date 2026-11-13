#!/usr/bin/env python3
"""编译真实 VM 日志块检查筛选边界，并用 v3 记录检查初始内存状态顺序。"""
import argparse
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
EXECUTION = ROOT / 'experiments/native-linux/translation/linux-execution.c'
MEMORY_INPUT = ROOT / 'scripts/fixtures/stock_init_vm_v3.json'
GUEST_KEYS = ('stock_init_syscall', 'stock_init_exec', 'stock_init_exec_layout', 'component_vm')
GIB = 1 << 30
CASES = (
    ('below_1gib', 222, GIB - 16384, 3, 0x22, 0x2000000, False),
    ('exact_1gib', 222, GIB, 3, 0x22, 0x2000000, True),
    ('actual_large_fixed_noreplace', 222, GIB + 262144, 3, 0x100022, 0x2000000, True),
    ('large_readonly', 222, GIB, 1, 0x22, 0x2000000, False),
    ('large_shared', 222, GIB, 3, 0x21, 0x2000000, False),
    ('large_shared_validate', 222, GIB, 3, 0x23, 0x2000000, False),
    ('large_private_file', 222, GIB, 3, 0x02, 0x2000000, False),
    ('mmap_failure', 222, 16384, 1, 0x02, -12, True),
    ('mprotect_failure', 226, 16384, 0, 0, -13, True),
    ('munmap_failure', 216, 16384, 0, 0, -22, True),
    ('mprotect_success', 226, GIB, 3, 0x22, 0, False),
    ('munmap_success', 216, GIB, 3, 0x22, 0, False),
    ('other_syscall_failure', 79, 16384, 0, 0, -2, False),
)
FIXTURE = r'''
#include <assert.h>
#include <stdio.h>

typedef struct { struct { unsigned long pc; } regs; } Process;

static long run_syscall(long number, const long *args)
{
    assert(number == 172);
    for (unsigned i = 0; i < 6; ++i)
        assert(args[i] == 0);
    return 4242;
}

static void trace_case(long number, long length, long prot, long flags, long result)
{
    int component_vm_trace = 1;
    Process process = {{0x100001000UL}}, *p = &process;
    long args[6] = {0x2000000, length, prot, flags, flags & 32 ? -1 : 5, 0};
    ACTUAL_VM_BLOCK
}

int main(void)
{
    _Static_assert(sizeof(long) == 8, "fixture requires 64-bit long");
    ACTUAL_CASES
    return 0;
}
'''


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def memory_cases():
    fixture = json.loads(MEMORY_INPUT.read_text())
    prepared = fixture['prepared']
    subsets = {phase: [json.loads(record['json_line']) for record in fixture['phases'][phase]['records']]
               for phase in ('lifecycle', 'recovery')}
    actual = subsets['lifecycle']
    vm = [row for row in actual if 'stock_init_vm_state' in row]
    others = [row for row in actual if 'stock_init_vm_state' not in row]
    guest = next(i for i, row in enumerate(others) if any(key in row for key in GUEST_KEYS))
    ready = next(i for i, row in enumerate(others) if 'stock_init_ramdisk_ready' in row)
    candidates = (
        ('actual_first_stage_order', actual, True, True),
        ('vm_after_guest_start', [*others[:guest + 1], *vm, *others[guest + 1:]], True, False),
        ('vm_before_ramdisk_ready', [*others[:ready], *vm, *others[ready:]], True, False),
        ('actual_recovery_order', subsets['recovery'], False, True),
    )
    checks = []
    for name, rows, first_stage, expected in candidates:
        state = probe._memory_state(rows, prepared, first_stage)
        accepted = (state is not None and state.get('overcommit_memory') == 0 and
                    state.get('memory_kib', {}).get('MemTotal') == 1042880 and
                    state.get('memory_kib', {}).get('SwapTotal') == 0)
        checks.append({'name': name, 'expected': expected, 'accepted': accepted,
                       'passed': accepted is expected, 'initial_memory_state': state})
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    common = Path(subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    main_root = common.resolve().parent
    output = args.output.resolve()
    if not output.is_relative_to(main_root / 'work/native-linux-nce'):
        parser.error('输出需要位于 MAIN/work/native-linux-nce')
    output.mkdir(parents=True, exist_ok=False)
    sources = (Path(__file__).resolve(), Path(probe.__file__).resolve(), EXECUTION, MEMORY_INPUT)
    before = {str(path): sha(path) for path in sources}
    result = {'test': 'stock_init_vm_observation', 'passed': False, 'host': platform.platform(),
              'source_sha256_before': before, 'checks': [],
              'scope': '宿主 C 编译本树真实 VM 日志块；仓库内保存的 v3 完整原 JSON 行用于初始内存状态顺序检查。'}
    try:
        source = EXECUTION.read_text()
        converted = probe._init_vm_trace_source(source)
        start = converted.index('if (component_vm_trace &&')
        end = converted.index('\n            if (number == NCE_SYS_PUBLISH_CODE', start)
        block = converted[start:end]
        result['converted_execution_sha256'] = hashlib.sha256(converted.encode()).hexdigest()
        result['vm_block_sha256'] = hashlib.sha256(block.encode()).hexdigest()
        calls = '\n'.join('    puts(%s);\n    trace_case(%d, %d, %d, %d, %d);' %
                          (json.dumps(json.dumps({'fixture_case': name})), number, length, prot, flags, returned)
                          for name, number, length, prot, flags, returned, _ in CASES)
        fixture = output / 'vm-fixture.c'
        fixture.write_text(FIXTURE.replace('ACTUAL_VM_BLOCK', block).replace('ACTUAL_CASES', calls))
        compiler = shutil.which('clang')
        if not compiler:
            raise RuntimeError('缺少 clang')
        result['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True, timeout=10).splitlines()[0]
        binary = output / 'vm-fixture'
        build = [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(fixture), '-o', str(binary)]
        result['build_command'] = build
        with (output / 'build.log').open('w') as log:
            subprocess.run(build, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
        result['fixture_command'] = [str(binary)]
        with (output / 'fixture.jsonl').open('w') as stdout, (output / 'fixture.stderr').open('w') as stderr:
            subprocess.run(result['fixture_command'], stdout=stdout, stderr=stderr, check=True, timeout=10)
        rows = probe.read_observations(output / 'fixture.jsonl')
        position = 0
        for name, number, length, prot, flags, returned, expected in CASES:
            marker = {'fixture_case': name}
            if position >= len(rows) or rows[position] != marker:
                raise ValueError('fixture 调用顺序变化：' + name)
            position += 1
            observed = rows[position] if position < len(rows) and 'component_vm' in rows[position] else None
            position += int(observed is not None)
            expected_row = {'component_vm': number, 'pid': 4242, 'pc': 0x100001000,
                            'args': [0x2000000, length, prot, flags, (1 << 64) - 1 if flags & 32 else 5, 0], 'result': returned}
            passed = observed == expected_row if expected else observed is None
            result['checks'].append({'name': name, 'expected_recorded': expected,
                                     'recorded': observed is not None, 'passed': passed, 'row': observed})
        if position != len(rows):
            raise ValueError('fixture 存在额外记录')
        result['checks'].extend(memory_cases())
    except (OSError, ValueError, RuntimeError, StopIteration, subprocess.SubprocessError) as error:
        result['error'] = str(error)
    result['source_sha256_after'] = {path: sha(Path(path)) for path in before}
    result['source_unchanged'] = before == result['source_sha256_after']
    result['passed'] = ('error' not in result and result['source_unchanged'] and
                        len(result['checks']) == len(CASES) + 4 and all(row['passed'] for row in result['checks']))
    result['artifacts'] = {path.name: {'bytes': path.stat().st_size, 'sha256': sha(path)}
                           for path in sorted(output.iterdir()) if path.is_file()}
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks']),
                      'report': str(output / 'result.json')}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
