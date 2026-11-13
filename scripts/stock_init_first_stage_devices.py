"""核对固定 SDK36 入口实际读取的 sysfs 设备与 GPT 分区。"""
import posixpath
import re

PLATFORM = '/sys/devices/platform/a003c00.virtio_mmio'
FIELDS = {'stock_init_first_stage_sysfs', 'operation', 'path_hex', 'value_hex',
          'open_results', 'read_results', 'close_result', 'readlink_results', 'result'}


def _integer(value, bits):
    return type(value) is int and 0 <= value < 1 << bits


def _hex(value, maximum):
    if type(value) is not str or len(value) > maximum * 2 or re.fullmatch('(?:[0-9a-f]{2})*', value) is None:
        raise ValueError('sysfs 十六进制字段格式错误')
    return bytes.fromhex(value)


def _attempts(values, final):
    return (type(values) is list and bool(values) and
            all(type(value) is int and value == -4 for value in values[:-1]) and
            type(values[-1]) is int and values[-1] == final)


def _value(row, operation, absent):
    if (set(row) != FIELDS or row['stock_init_first_stage_sysfs'] is not True or
            row['operation'] != operation or type(row['result']) is not int):
        raise ValueError('sysfs 查询记录格式错误')
    value = _hex(row['value_hex'], 4096)
    if absent:
        if (row['result'] != -2 or value or row['open_results'] is not None or
                row['read_results'] is not None or row['close_result'] is not None or
                not _attempts(row['readlink_results'], -2)):
            raise ValueError('sysfs 预期缺失的链接状态不同')
        return None
    if row['result'] != 0:
        raise ValueError('sysfs 查询失败')
    if operation == 'readlink':
        if (not value or row['open_results'] is not None or row['read_results'] is not None or
                row['close_result'] is not None or not _attempts(row['readlink_results'], len(value))):
            raise ValueError('sysfs readlink 返回与内容不同')
    else:
        opened, reads = row['open_results'], row['read_results']
        if (type(opened) is not list or not opened or not _integer(opened[-1], 31) or
                not _attempts(opened, opened[-1]) or type(reads) is not list or not reads or
                type(row['close_result']) is not int or row['close_result'] != 0 or
                row['readlink_results'] is not None or type(reads[-1]) is not int or reads[-1] != 0):
            raise ValueError('sysfs 文件读取或关闭记录错误')
        total = 0
        for result in reads[:-1]:
            if type(result) is not int or (result != -4 and not 0 < result <= max(4096 - total, 1)):
                raise ValueError('sysfs 短读记录错误')
            if result > 0:
                total += result
        if total != len(value):
            raise ValueError('sysfs read 返回与内容长度不同')
    if b'\0' in value:
        raise ValueError('sysfs 内容含 NUL')
    return value.decode('ascii')


def _number(value, bits=64, newline=True):
    pattern = r'(0|[1-9][0-9]*)\n' if newline else r'(0|[1-9][0-9]*)'
    if type(value) is not str or re.fullmatch(pattern, value) is None:
        raise ValueError('sysfs 十进制字段格式错误')
    number = int(value)
    if not _integer(number, bits):
        raise ValueError('sysfs 十进制字段超出范围')
    return number


def _link(path, value):
    if not value or '\n' in value:
        raise ValueError('sysfs 链接内容错误')
    return posixpath.normpath(posixpath.join(posixpath.dirname(path), value))


def _device(value, uevent, name, partition=False):
    if re.fullmatch(r'(0|[1-9][0-9]*):(0|[1-9][0-9]*)\n', value) is None:
        raise ValueError('sysfs dev 格式错误')
    major, minor = map(int, value.strip().split(':'))
    if not 0 < major < 4096 or not _integer(minor, 20):
        raise ValueError('sysfs dev 超出 Linux dev_t 范围')
    fields = {}
    if not uevent.endswith('\n'):
        raise ValueError('sysfs uevent 未完整读取')
    for line in uevent.splitlines():
        key, separator, content = line.partition('=')
        if not separator or key in fields:
            raise ValueError('sysfs uevent 字段重复或格式错误')
        fields[key] = content
    expected = {'MAJOR', 'MINOR', 'DEVNAME', 'DEVTYPE', 'DISKSEQ'}
    if partition:
        expected |= {'PARTN', 'PARTNAME'}
    if (set(fields) != expected or fields['MAJOR'] != str(major) or fields['MINOR'] != str(minor) or
            fields['DEVNAME'] != name or fields['DEVTYPE'] != ('partition' if partition else 'disk')):
        raise ValueError('sysfs dev 与 uevent 设备身份不同')
    sequence = _number(fields['DISKSEQ'], newline=False)
    if not sequence:
        raise ValueError('sysfs DISKSEQ 为零')
    device = (major << 8) | (minor & 255) | ((minor & ~255) << 12)
    return {'major': major, 'minor': minor, 'device': device, 'diskseq': sequence, 'uevent': fields}


def observe(rows, inputs, ranchu):
    if type(ranchu) is not bool:
        raise ValueError('sysfs 设备模式格式错误')
    records = [row for row in rows if 'stock_init_first_stage_sysfs' in row]
    paths = [_hex(row.get('path_hex'), 160).decode('ascii') for row in records]
    if len(set(paths)) != len(paths):
        raise ValueError('sysfs 查询路径重复')
    queries = dict(zip(paths, records))
    expected = []

    def value(path, operation='read', absent=False):
        expected.append((operation, path))
        if path not in queries:
            raise ValueError('缺少 sysfs 查询：' + path)
        return _value(queries[path], operation, absent)

    platform = value(PLATFORM + '/subsystem', 'readlink', not ranchu)
    if ranchu and _link(PLATFORM + '/subsystem', platform) != '/sys/bus/platform':
        raise ValueError('sysfs platform subsystem 不同')
    value('/sys/class/block/' + ('ncedisk0' if ranchu else 'vdc'), 'readlink', True)
    data, metadata = inputs.get('data_image'), inputs.get('metadata_image')
    disk_inputs = [(0, 'vdc' if ranchu else 'ncedisk0', data, 0),
                   (1, 'ncedisk1', {'bytes': inputs['lp_metadata']['image_bytes']}, 1),
                   (2, 'ncedisk2', metadata, 0)]
    devices = {}
    for slot, name, image, readonly in disk_inputs:
        path = '/sys/class/block/' + name
        target = value(path, 'readlink', not bool(image))
        if not image:
            continue
        canonical = (PLATFORM if ranchu else '/sys/devices/virtual') + '/block/' + name
        if _link(path, target) != canonical:
            raise ValueError('sysfs 整盘 canonical 路径不同')
        device = _device(value(path + '/dev'), value(path + '/uevent'), name)
        ro, sectors = _number(value(path + '/ro')), _number(value(path + '/size'))
        parent = value(path + '/device', 'readlink', not ranchu)
        if (device['major'] == 259 or device['minor'] != slot or ro != readonly or
                sectors * 512 != image['bytes'] or
                (ranchu and _link(canonical + '/device', parent) != PLATFORM)):
            raise ValueError('sysfs 整盘属性或父设备不同')
        devices[name] = {**device, 'canonical': canonical, 'slot': slot, 'readonly': ro, 'sectors': sectors}
    if len({item['major'] for item in devices.values()}) != 1 or len({item['diskseq'] for item in devices.values()}) != len(devices):
        raise ValueError('sysfs 整盘 major 或 DISKSEQ 不一致')
    # system GPT 范围来自固定 SDK36 原盘；metadata 使用本轮输入检查结果。
    partition_inputs = [('ncedisk1p1', 'ncedisk1', 1, 'vbmeta', 2048, 2048),
                        ('ncedisk1p2', 'ncedisk1', 2, 'super', 4096, 8617984)]
    part = metadata['partition'] if metadata else None
    partition_inputs.append(('ncedisk2p1', 'ncedisk2', 1, 'metadata',
                             part['start_sector'] if part else 0, part['sectors'] if part else 0))
    for name, disk, number, partname, start, sectors in partition_inputs:
        path = '/sys/class/block/' + name
        target = value(path, 'readlink', disk not in devices)
        if disk not in devices:
            continue
        parent = devices[disk]
        canonical = parent['canonical'] + '/' + name
        if _link(path, target) != canonical:
            raise ValueError('sysfs GPT 分区 canonical 路径不同')
        device = _device(value(path + '/dev'), value(path + '/uevent'), name, True)
        ro, actual_number = _number(value(path + '/ro')), _number(value(path + '/partition'))
        actual_start, actual_size = _number(value(path + '/start')), _number(value(path + '/size'))
        subsystem = value(path + '/subsystem', 'readlink')
        if (device['major'] != 259 or ro != parent['readonly'] or actual_number != number or
                device['uevent']['PARTN'] != str(number) or device['uevent']['PARTNAME'] != partname or
                device['diskseq'] != parent['diskseq'] or actual_start != start or actual_size != sectors or
                _link(canonical + '/subsystem', subsystem) != '/sys/class/block'):
            raise ValueError('sysfs GPT 分区属性或 subsystem 不同')
        devices[name] = {**device, 'canonical': canonical, 'readonly': ro, 'partition': number,
                         'start_sector': start, 'sectors': sectors}
    if len({item['device'] for item in devices.values()}) != len(devices):
        raise ValueError('sysfs 设备号重复')
    if [(row.get('operation'), path) for row, path in zip(records, paths)] != expected:
        raise ValueError('sysfs 查询数量或顺序不同')
    summaries = [row for row in rows if 'stock_init_first_stage_devices' in row]
    summary = {'stock_init_first_stage_devices': True, 'ranchu': int(ranchu),
               'data': int(bool(data)), 'metadata': int(bool(metadata)), 'result': 0}
    if (summaries != [summary] or summaries[0]['stock_init_first_stage_devices'] is not True or
            any(type(summaries[0][key]) is not int for key in ('ranchu', 'data', 'metadata', 'result'))):
        raise ValueError('sysfs 设备查询汇总不同')
    mounts = [row for row in rows if 'stock_init_first_stage_sysfs_mount' in row]
    operations = ['mount', 'unmount'] if ranchu else ['mount']
    if (mounts != [{'stock_init_first_stage_sysfs_mount': True, 'operation': operation, 'result': 0} for operation in operations] or
            any(row['stock_init_first_stage_sysfs_mount'] is not True or type(row['result']) is not int for row in mounts)):
        raise ValueError('sysfs 挂载生命周期不同')
    events = [mounts[0], *records, summaries[0], *mounts[1:]]
    positions = [rows.index(row) for row in events]
    if any(before >= after for before, after in zip(positions, positions[1:])):
        raise ValueError('sysfs 查询与挂载顺序不同')
    return devices


def validate(rows, inputs, ranchu):
    try:
        observe(rows, inputs, ranchu)
        return True
    except (ValueError, TypeError, KeyError, IndexError, UnicodeError):
        return False
