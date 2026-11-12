#!/usr/bin/env python3
"""检查 JIT 地址关联的拒绝条件；构造记录不执行 ART。"""
from copy import deepcopy
from pathlib import Path
import tempfile

from stock_art_jit import observe


def main():
    base = {'pid': 2, 'tid': 2, 'mm': 123456}
    rows = [dict(base, execution_observation='marker', sequence=1, name='art-jit-begin', pending_vm_calls=0),
            dict(base, execution_observation='integer_completed', sequence=2, pc=0x10004, next_pc=0x23400),
            dict(base, execution_observation='marker', sequence=3, name='art-jit-end', pending_vm_calls=0)]
    line = '00010000-00014000 r-xs 02000000 00:01 2 /memfd:jit-cache (deleted)\n'
    with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1] / 'work', prefix='jit-observe-') as directory:
        directory = Path(directory)
        for name in ('before-warmup', 'after-warmup'):
            (directory / f'maps-{name}.txt').write_text(line)
        assert observe(directory, rows)['jit_execution_confirmed']
        rejected = 0
        for index, key, value in [(1, 'pc', 0x23400), (1, 'pc', 0x14000), (1, 'pc', True),
                                  (1, 'pc', 0x10005), (1, 'pid', 3), (1, 'tid', 3), (1, 'mm', 654321),
                                  (1, 'sequence', 3), (2, 'mm', 654321), (2, 'name', 'art-jit-begin'),
                                  (0, 'pending_vm_calls', 1), (2, 'pending_vm_calls', 1), (0, 'pending_vm_calls', False)]:
            changed = deepcopy(rows)
            changed[index][key] = value
            assert not observe(directory, changed)['jit_execution_confirmed'], (index, key, value)
            rejected += 1
        for index in range(len(rows)):
            assert not observe(directory, rows[:index] + rows[index + 1:])['jit_execution_confirmed']
            rejected += 1
        for content in (line.replace('r-xs', 'rw-s'), line.replace('00:01 2', '00:01 3'),
                        line.replace('02000000', '01000000'), 'invalid\n'):
            (directory / 'maps-after-warmup.txt').write_text(content)
            assert not observe(directory, rows)['jit_execution_confirmed']
            rejected += 1
        (directory / 'maps-after-warmup.txt').write_text(line)
        for number, args in [(215, [0x10000, 1, 0, 0, 0, 0]), (226, [0x10000, 1, 5, 0, 0, 0]),
                             (222, [0x10000, 0x4000, 5, 0x12, 9, 0]), (216, [0x10000, 0, 0x4000, 1, 0, 0]),
                             (216, [0x20000, 0x4000, 0x4000, 3, 0x10000, 0]),
                             (233, [0x10000, 0x4000, 4, 0, 0, 0]), (234, [0x10000, 0x4000, 0, 0, 0, 0]),
                             (288, [0x10000, 0x4000, 5, 0, 0, 0]), (440, [4, 0, 0, 4, 0, 0]),
                             (221, [0, 0, 0, 0, 0, 0])]:
            changed = deepcopy(rows)
            changed.insert(1, dict(base, execution_observation='vm_call', sequence=2, number=number, args=args))
            changed[2]['sequence'] = 3
            changed[3]['sequence'] = 4
            assert not observe(directory, changed)['jit_execution_confirmed'], (number, args)
            rejected += 1
            changed[1]['pid'] = 3
            assert not observe(directory, changed)['jit_execution_confirmed']
            rejected += 1
            changed[1]['mm'] = 987654
            assert observe(directory, changed)['jit_execution_confirmed'] == (number != 440)
        changed = deepcopy(rows)
        changed[1]['pc'], changed[1]['next_pc'] = 0x23400, 0x10004
        assert not observe(directory, changed)['jit_execution_confirmed']
        rejected += 1
        fault = dict(base, execution_observation='guest_fault', sequence=2, pc=0x10008,
                     address=8, access=0, result=-14)
        faults_only = [rows[0], fault, rows[-1]]
        observed = observe(directory, faults_only)
        assert observed['guest_faults'] == [fault] and observed['samples'] == 0
        assert not observed['jit_execution_confirmed'] and observed['error'] is None
        combined = [rows[0], fault, dict(rows[1], sequence=3), dict(rows[-1], sequence=4)]
        assert observe(directory, combined)['jit_execution_confirmed']
        for key, value in [('pid', 3), ('tid', 3), ('mm', 654321), ('pc', True), ('pc', 0x10001),
                           ('address', -1), ('address', 1 << 64), ('access', True), ('access', 3),
                           ('result', False), ('result', 0)]:
            changed = deepcopy(combined)
            changed[1][key] = value
            assert observe(directory, changed)['error'] is not None, (key, value)
            assert not observe(directory, changed)['jit_execution_confirmed']
            rejected += 1
        changed = [rows[0], rows[1], rows[2], dict(fault, sequence=4)]
        assert observe(directory, changed)['error'] is not None
        rejected += 1
        for page_bytes in (0, 8192, True, '4096'):
            observed = observe(directory, rows, guest_page_bytes=page_bytes)
            assert observed['error'] and not observed['jit_execution_confirmed']
            rejected += 1
        adjacent = line.replace('00010000-00014000', '00011000-00012000')
        for name in ('before-warmup', 'after-warmup'):
            (directory / f'maps-{name}.txt').write_text(adjacent)
        page_rows = deepcopy(rows)
        page_rows[1]['pc'] = 0x11004
        boundaries = 0
        for number, remap_destination in ((215, False), (226, False), (233, False), (234, False),
                                          (288, False), (222, False), (216, False), (216, True)):
            for address, length, invalidated in ((0x10000, 1, False), (0x10000, 0x1000, False),
                                                  (0x10000, 0x1001, True), (0x11000, 1, True),
                                                  (0x12000, 1, False)):
                args = [address, length, 5, 0x12, 9, 0]
                if number == 216:
                    args = ([0x30000, 0x1000, length, 3, address, 0] if remap_destination else
                            [address, length, 0x1000, 1, 0, 0])
                changed = [page_rows[0], dict(base, execution_observation='vm_call', sequence=2,
                                             number=number, args=args),
                           dict(page_rows[1], sequence=3), dict(page_rows[-1], sequence=4)]
                observed = observe(directory, changed, guest_page_bytes=4096)
                assert observed['error'] is None and observed['samples'] == 1
                assert observed['candidates'][0]['invalidating_calls'] == ([2] if invalidated else []), (number, args)
                assert observed['jit_execution_confirmed'] == (not invalidated), (number, args)
                # The default keeps the existing 16 KiB range contract.
                default = observe(directory, changed)
                explicit = observe(directory, changed, guest_page_bytes=16384)
                assert default == explicit
                assert default['candidates'][0]['invalidating_calls'] == ([2] if address < 0x12000 else [])
                boundaries += 1
        (directory / 'maps-before-warmup.txt').unlink()
        assert not observe(directory, rows)['jit_execution_confirmed']
        rejected += 1
    print(f'JIT observation parser: {rejected} rejection cases and {boundaries} page boundary cases passed; no ART execution')


if __name__ == '__main__':
    main()
