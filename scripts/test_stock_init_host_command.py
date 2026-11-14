#!/usr/bin/env python3
"""检查原 init 从 core 编译记录恢复 C 输入和保留链接参数。"""
from copy import deepcopy
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from probe_stock_init import EXP, _host_source_command


def main():
    core = Path('/core')
    sources = [str(EXP / 'translation' / name)
               for name in ('linux-runner.c', 'linux-execution.c', 'linux-elf.c')]
    objects = [str(core / f'host-{index}.o') for index in range(len(sources))]
    direct = str(core / 'direct/execute.o')
    suffix = ['-o', str(core / 'gcd'), '-DNCE_DIRECT_EXECUTION', '-DTRANSLATION_DISPATCH',
              '-DNCE_GUEST_PAGE_SHIFT=14', '-iquote', str(EXP / 'translation')]
    prefix = ['xcrun', 'clang', '-O2', '-I', str(core)]
    libraries = [direct, '/unicorn/libunicorn.a', '-lc++']
    expected = [*prefix, *sources, *libraries, *suffix]
    commands = [['xcrun', 'clang', '-c', source, '-o', target]
                for source, target in zip(sources, objects)]
    commands.append(['xcrun', 'clang++', '-c', str(EXP / 'direct/execute.cpp'), '-o', direct])
    commands.append([*prefix, *objects, *libraries, *suffix])
    base = {'commands': commands}
    before = deepcopy(base)
    checks = []

    def check(name, condition):
        checks.append({'name': name, 'passed': bool(condition)})

    restored = _host_source_command(base, core, 'gcd')
    check('split-c-compile-and-link', restored == expected)
    check('direct-object-retained', restored.count(direct) == 1)
    check('trailing-macros-and-includes-retained', restored[-len(suffix):] == suffix)
    check('core-report-retained', base == before)
    historical = {'commands': [expected]}
    check('historical-c-source-command', _host_source_command(historical, core, 'gcd') == expected)
    cases = {
        'missing-host-link': {'commands': commands[:-1]},
        'duplicate-host-link': {'commands': [*commands, commands[-1]]},
        'conflicting-c-object': {'commands': [*commands,
            ['xcrun', 'clang', '-c', str(EXP / 'translation/other.c'), '-o', objects[0]]]},
        'missing-c-source': {'commands': commands[1:]},
        'duplicate-c-source': {'commands': [[*prefix, *sources, sources[0], *libraries, *suffix]]},
    }
    for name, candidate in cases.items():
        try:
            _host_source_command(candidate, core, 'gcd')
        except ValueError:
            check(name, True)
        else:
            check(name, False)
    passed = all(item['passed'] for item in checks)
    print(json.dumps({'passed': passed, 'checks': checks}, ensure_ascii=False, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
