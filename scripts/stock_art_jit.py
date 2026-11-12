"""关联已完成整数指令、观测区间与原始 ART JIT maps。"""
import re


def mappings(path):
    result = []
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]+)-([0-9a-f]+) ([rwxps-]{4}) ([0-9a-f]+) ([0-9a-f]+:[0-9a-f]+) (\d+)\s*(.*)', line)
        if not match:
            raise ValueError('maps 行格式错误')
        start, end, permissions, offset, device, inode, name = match.groups()
        row = {'start': int(start, 16), 'end': int(end, 16), 'permissions': permissions,
               'offset': int(offset, 16), 'device': device, 'inode': int(inode), 'name': name}
        if row['start'] >= row['end']:
            raise ValueError('maps 范围错误')
        result.append(row)
    if any(a['end'] > b['start'] for a, b in zip(result, result[1:])):
        raise ValueError('maps 范围重叠或顺序错误')
    return result


def overlaps(address, length, candidate):
    # Linux rounds these VM lengths to its 16 KiB page size.
    length = (length + 16383) & ~16383
    return address < candidate['end'] and candidate['start'] < address + length


def may_replace(event, candidate):
    number, args = event['number'], event['args']
    if number in (215, 226, 233, 234, 288):
        return overlaps(args[0], args[1], candidate)
    if number == 222:
        return bool(args[3] & (0x10 | 0x100000)) and overlaps(args[0], args[1], candidate)
    if number == 216:
        return (overlaps(args[0], max(args[1], 1), candidate) or
                bool(args[3] & 2) and overlaps(args[4], args[2], candidate))
    # brk cannot replace an existing memfd mapping. Other supported operations
    # have no sufficient range contract here and invalidate every candidate.
    return number != 214


def observe(directory, rows):
    result = {'jit_execution_confirmed': False, 'scope': 'Completed integer instruction within a stable ART JIT RX mapping; method identity unverified',
              'error': None, 'samples': 0, 'guest_faults': [], 'candidates': [], 'confirmed': []}
    try:
        events = [row for row in rows if 'execution_observation' in row]
        if not events:
            raise ValueError('缺少宿主执行观测')
        common = {'execution_observation', 'sequence', 'pid', 'tid', 'mm'}
        for index, row in enumerate(events, 1):
            if any(type(row.get(key)) is not int or row[key] <= 0 for key in ('sequence', 'pid', 'tid', 'mm')) or row['sequence'] != index:
                raise ValueError('宿主观测身份或顺序错误')
            kind = row['execution_observation']
            if kind == 'integer_completed':
                if set(row) != common | {'pc', 'next_pc'} or any(type(row[key]) is not int or not 0 <= row[key] < 1 << 64 or row[key] % 4 for key in ('pc', 'next_pc')):
                    raise ValueError('已完成指令 PC 格式错误')
            elif kind == 'guest_fault':
                if (set(row) != common | {'pc', 'address', 'access', 'result'} or
                        any(type(row[key]) is not int or not 0 <= row[key] < 1 << 64 for key in ('pc', 'address')) or
                        row['pc'] % 4 or type(row['access']) is not int or row['access'] not in (0, 1, 2) or
                        type(row['result']) is not int or row['result'] == 0):
                    raise ValueError('客体访存故障格式错误')
            elif kind == 'marker':
                if (set(row) != common | {'name', 'pending_vm_calls'} or row['name'] not in ('art-jit-begin', 'art-jit-end') or
                        type(row['pending_vm_calls']) is not int or row['pending_vm_calls'] != 0):
                    raise ValueError('观测标记错误')
            elif kind == 'vm_call':
                if (set(row) != common | {'number', 'args'} or type(row['number']) is not int or
                        row['number'] not in (222, 226, 216, 215, 214, 221, 196, 197, 233, 281, 282, 234, 288, 440) or
                        type(row['args']) is not list or len(row['args']) != 6 or
                        any(type(value) is not int or not 0 <= value < 1 << 64 for value in row['args'])):
                    raise ValueError('VM 调用格式错误')
            else:
                raise ValueError('未知宿主观测')
        markers = [row for row in events if row['execution_observation'] == 'marker']
        if len(markers) != 2 or [row['name'] for row in markers] != ['art-jit-begin', 'art-jit-end']:
            raise ValueError('观测窗口缺失或重复')
        begin, end = markers
        identity = {key: begin[key] for key in ('pid', 'tid', 'mm')}
        if any(end[key] != value for key, value in identity.items()):
            raise ValueError('观测窗口进程或地址空间发生变化')
        window = [row for row in events if begin['sequence'] < row['sequence'] < end['sequence']]
        samples = [row for row in events if row['execution_observation'] == 'integer_completed']
        faults = [row for row in events if row['execution_observation'] == 'guest_fault']
        if any(not begin['sequence'] < row['sequence'] < end['sequence'] or
               any(row[key] != value for key, value in identity.items()) for row in samples + faults):
            raise ValueError('指令观测超出目标线程或窗口')
        changes = [row for row in window if row['execution_observation'] == 'vm_call']
        if any(row['pid'] == begin['pid'] and row['mm'] != begin['mm'] for row in changes):
            raise ValueError('VM 调用地址空间发生变化')
        changes = [row for row in changes if row['mm'] == begin['mm'] or row['number'] == 440]
        before = mappings(directory / 'maps-before-warmup.txt')
        after = mappings(directory / 'maps-after-warmup.txt')
        candidates = [row for row in before if row['name'] == '/memfd:jit-cache (deleted)' and
                      row['permissions'] == 'r-xs' and row['inode'] > 0 and row in after]
        result.update(identity=identity, window=[begin['sequence'], end['sequence']], samples=len(samples), guest_faults=faults)
        for candidate in candidates:
            mutations = [row['sequence'] for row in changes if may_replace(row, candidate)]
            hits = [row for row in samples if candidate['start'] <= row['pc'] < candidate['end']]
            item = dict(candidate, invalidating_calls=mutations, completed_samples=len(hits))
            result['candidates'].append(item)
            if hits and not mutations:
                result['confirmed'].append(dict(item, first=hits[0], last=hits[-1]))
        result['jit_execution_confirmed'] = bool(result['confirmed'])
    except (ValueError, OSError, KeyError, TypeError) as error:
        result['error'] = str(error)
    return result
