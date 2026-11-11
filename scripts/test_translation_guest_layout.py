#!/usr/bin/env python3
"""检查低客体布局的配置范围、映射身份和不兼容参数。"""
from copy import deepcopy
from pathlib import Path
import subprocess
import sys

from probe_lkl_translation import validate_guest_layout


def main():
    low = {'config': 'translation_guest_layout', 'mode': 'low',
           'guest_base': 0x10000, 'guest_end': 1 << 32, 'guest_span_bytes': (1 << 32) - 0x10000,
           'flat_coverage_bytes': 1 << 32, 'physical_base': 8 << 30, 'ram_base': 16 << 30,
           'ram_bytes': 1 << 30, 'reservation_base': 8 << 30, 'reservation_bytes': (1 << 30) + (64 << 20)}
    host = dict(low, mode='host', guest_base=8 << 30, guest_end=12 << 30,
                guest_span_bytes=1 << 32, physical_base=12 << 30,
                reservation_bytes=(5 << 30) + (64 << 20))
    rejected = 0

    def reject(rows, mode, flat=True):
        nonlocal rejected
        assert not validate_guest_layout(rows, mode, flat), (rows, mode, flat)
        rejected += 1

    for row in (host, low):
        mode = row['mode']
        assert validate_guest_layout([row], mode, True)
        reject([], mode)
        reject([row, deepcopy(row)], mode)
        reject([row], 'low' if mode == 'host' else 'host')
        reject([dict(row, extra=True)], mode)
        for key, value in row.items():
            missing = dict(row)
            del missing[key]
            reject([missing], mode)
            if type(value) == int:
                for bad in (-1, value + 1, True, str(value), 1 << 64):
                    reject([dict(row, **{key: bad})], mode)
        reject([dict(row, ram_base=row['physical_base'])], mode)
        reject([dict(row, ram_base=row['guest_base'])], mode)
    assert validate_guest_layout([dict(host, flat_coverage_bytes=0)], 'host', False)
    reject([low], 'low', False)
    reject([dict(low, guest_end=(1 << 32) + 0x10000, guest_span_bytes=1 << 32)], 'low')
    reject([dict(low, reservation_base=(1 << 40) - 16384, physical_base=(1 << 40) - 16384)], 'low')
    script = Path(__file__).with_name('probe_lkl_translation.py')
    common = [sys.executable, script, '--guest-layout', 'low', '--mapping', 'pte',
              '--execution', 'direct', '--direct-memory', 'flat', '--cpus', '1', '--build-only']
    for option, value in (('--mapping', 'window'), ('--execution', 'unicorn'),
                          ('--direct-memory', 'view'), ('--cpus', '2')):
        command = list(common)
        command[command.index(option) + 1] = value
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        assert result.returncode == 2 and '低客体地址实验需要单 CPU 的 pte/direct/flat' in result.stderr
        rejected += 1
    print(f'translation guest layout: valid host/low configurations and {rejected} negative cases passed')


if __name__ == '__main__':
    main()
