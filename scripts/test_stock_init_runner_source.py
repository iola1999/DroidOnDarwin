#!/usr/bin/env python3
"""检查真实 first-stage 运行器的源码转换与熵声明拒绝条件。"""
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from probe_stock_init import _first_stage_runner_source

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    paths = [Path(__file__), ROOT / 'scripts/probe_stock_init.py',
             ROOT / 'experiments/native-linux/translation/linux-runner.c']
    before = {str(path.relative_to(ROOT)): digest(path) for path in paths}
    source = paths[-1].read_text()
    start = source.index('static void stock_bionic_setup(void)')
    end = source.index('#include "stock-art-runner.h"', start)
    source = source[:start] + source[end:]
    entropy = 'long random[6] = {-100, (long)"/dev/random", 2};'
    device = 'long random_device[6] = {-100, (long)"/dev/random", 0020666, 264};'
    converted = _first_stage_runner_source(source)
    checks = []

    def check(name, passed):
        checks.append({'name': name, 'passed': bool(passed)})

    check('actual-two-random-path-uses', source.count('(long)"/dev/random"') == 2)
    check('first-stage-start-abi',
          converted.count('int (*start)(unsigned long, unsigned long *, unsigned, const char *) = (void *)(kernel + NCE_LKL_START_CPUS_CMDLINE);') == 1)
    check('runtime-cmdline-selection',
          converted.count('start(scheduler_slice_us * 1000, &slice, online_cpus, stock_init_first_stage_enabled() ? "androidboot.hardware=ranchu" : NULL)') == 1)
    check('runtime-entropy-selection',
          converted.count('long random[6] = {-100, (long)(stock_init_first_stage_enabled() ? "/dev/urandom" : "/dev/random"), 2};') == 1)
    check('random-device-declaration-preserved', source.count(device) == converted.count(device) == 1)
    check('entropy-forward-declaration',
          converted.count('static int stock_init_first_stage_enabled(void);\n\nstatic void seed_entropy(void)') == 1)
    for name, modified in (('missing-entropy-declaration', source.replace(entropy, '')),
                           ('duplicate-entropy-declaration', source + '\n' + entropy)):
        try:
            _first_stage_runner_source(modified)
        except ValueError as error:
            check(name, str(error) == '运行器启动参数或熵来源位置已变化')
        else:
            check(name, False)
    check('production-inputs-unchanged', all(digest(ROOT / name) == value for name, value in before.items()))
    passed = all(item['passed'] for item in checks)
    print(json.dumps({'passed': passed, 'checks': checks, 'sha256': before,
                      'scope': '真实运行器源码的宿主转换；客体执行与 C 编译由 raw 和 iOS 阶段检查。'}, ensure_ascii=False, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
