"""检查 SDK 原 GPT metadata 分区的挂载、目录属性与同盘持久化记录。"""
import hashlib
import re
from stock_init_data import inspect as inspect_ext4, validate_volume, validate_disk_cleanup

ORIGINAL_SHA = '522bd0b6a2c92e37de465059bb9487bb9597a9aae032b469a8ebe0b38146d8c3'
PARTITION = {'name': 'metadata', 'partition': 1, 'start_sector': 2048, 'sectors': 32768, 'whole_sectors': 36864}
DIRECTORIES = ('/metadata', '/metadata/vold', '/metadata/apex', '/metadata/apex/sessions')


def inspect(path):
    state = inspect_ext4(path, offset=PARTITION['start_sector'] * 512)
    if (state['bytes'] != PARTITION['whole_sectors'] * 512 or state['filesystem_bytes'] != PARTITION['sectors'] * 512 or
            state['uuid'] != 'b08db4c1cfe546f0b32af06729e6a3f2' or state['block_bytes'] != 4096):
        raise ValueError('SDK metadata 原分区格式或范围不同：' + str(path))
    # GPT 外围摘要覆盖分区前后字节，运行期分区识别由 Linux 完成。
    offset = PARTITION['start_sector'] * 512
    end = offset + PARTITION['sectors'] * 512
    with path.open('rb') as stream:
        prefix = hashlib.sha256(stream.read(offset)).hexdigest()
        stream.seek(end)
        suffix = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {**state, 'partition': dict(PARTITION), 'gpt_prefix_sha256': prefix, 'gpt_suffix_sha256': suffix}


def validate(rows, enabled, mode, data_enabled=False, contract=None):
    kinds = ('stock_init_metadata_partition', 'stock_init_metadata_directory', 'stock_init_metadata_directory_sync')
    groups = [[row for row in rows if kind in row] for kind in kinds]
    if not validate_volume(rows, 'metadata', enabled, mode):
        return False
    if not enabled:
        return not any(groups)
    if (contract != PARTITION or not validate_disk_cleanup(rows, data_enabled, True) or
            [len(group) for group in groups] != [1, 4, 1]):
        return False
    partition, directories, synced = groups[0][0], groups[1], groups[2][0]
    expected = {'stock_init_metadata_partition': True, **PARTITION,
                'whole_device': partition.get('whole_device'), 'device': partition.get('device'),
                'bytes': PARTITION['sectors'] * 512, 'readonly': 0, 'superblock_equal': True,
                'ext4_magic': 0xef53, 'end_read_result': 0, 'beyond_read_result': 0}
    if (partition != expected or partition['stock_init_metadata_partition'] is not True or
            partition['superblock_equal'] is not True or
            any(type(value) is not int for key, value in partition.items() if key not in ('name', 'stock_init_metadata_partition', 'superblock_equal')) or
            partition['device'] >> 32 != 259 or not 0 < partition['whole_device'] >> 32 < 4096 or partition['device'] & 0xffffffff >= 1 << 20 or
            partition['whole_device'] & 0xffffffff != 2 or partition['device'] == partition['whole_device']):
        return False
    mount = next(row for row in rows if 'stock_init_metadata_mount' in row)
    held = next(row for row in rows if 'stock_init_metadata_retained' in row)
    cleanup = next(row for row in rows if 'stock_init_metadata_cleanup' in row)
    reaped = [row for row in rows if 'stock_init_host_stop_reaped' in row]
    if (partition['device'] != mount['device'] or len(reaped) != 1 or
            synced != {'stock_init_metadata_directory_sync': True, 'fd': held['fd'], 'result': 0} or
            synced['stock_init_metadata_directory_sync'] is not True or
            any(type(synced[key]) is not int for key in ('fd', 'result'))):
        return False
    ordered = (partition, mount, reaped[0], *directories, synced, cleanup)
    positions = [rows.index(row) for row in ordered]
    if any(before >= after for before, after in zip(positions, positions[1:])):
        return False
    data_cleanup = [row for row in rows if 'stock_init_data_cleanup' in row]
    if data_cleanup and rows.index(data_cleanup[0]) <= rows.index(cleanup):
        return False
    fields = {'stock_init_metadata_directory', 'path', 'stat_result', 'device', 'mode', 'inode_type', 'uid', 'gid', 'label_result', 'label_hex'}
    for path, row in zip(DIRECTORIES, directories):
        if (set(row) != fields or row['stock_init_metadata_directory'] is not True or row['path'] != path or
                any(type(row[key]) is not int for key in fields - {'stock_init_metadata_directory', 'path', 'label_hex'}) or
                not -4095 <= row['stat_result'] <= 0 or not -4095 <= row['label_result'] <= 256 or
                type(row['label_hex']) is not str or re.fullmatch('[0-9a-f]{%d}' % (max(row['label_result'], 0) * 2), row['label_hex']) is None):
            return False
        if (row['stat_result'] not in ((0, -2) if mode == 'lifecycle' else (0, -13)) or
                (row['stat_result'] != 0 and any(row[key] != 0 for key in ('device', 'mode', 'inode_type', 'uid', 'gid'))) or
                (row['stat_result'] == -13 and row['label_result'] != -13)):
            return False
        if row['stat_result'] == 0 and (row['device'] != partition['device'] or row['inode_type'] != 0o40000):
            return False
        if path == '/metadata' and row['stat_result'] not in ((0,) if mode == 'lifecycle' else (0, -13)):
            return False
    return True


def rc_results(rows):
    attributes = [row for row in rows if 'stock_init_metadata_directory' in row]
    sessions = [row for row in rows if row.get('stock_init_block') == 56 and
                row.get('paths_hex') == ['/metadata/apex/sessions'.encode().hex()] and
                type(row.get('pid')) is int and type(row.get('result')) is int]
    result = {'directory_contract': {}, 'directories_match_original_rc': None, 'attributes_denied': False,
              'apex_sessions_opens': [{'pid': row['pid'], 'result': row['result']} for row in sessions],
              'apex_sessions_opened': any(row['pid'] > 1 and row['result'] >= 0 for row in sessions)}
    fields = ('stat_result', 'uid', 'gid', 'mode', 'label_result')
    if (len(attributes) != 4 or [row.get('path') for row in attributes] != list(DIRECTORIES) or
            any(any(type(row.get(key)) is not int for key in fields) or type(row.get('label_hex')) is not str for row in attributes)):
        result['error'] = 'metadata 目录观察字段无效'
        return result
    expected = ((0, 0, 0o755, 'metadata_file'), (0, 0, 0o700, 'vold_metadata_file'),
                (0, 1000, 0o700, 'apex_metadata_file'), (0, 1000, 0o700, 'apex_metadata_file'))
    matched = {}
    for path, (uid, gid, mode, label), row in zip(DIRECTORIES, expected, attributes):
        matched[path] = None if -13 in (row['stat_result'], row['label_result']) else (row['stat_result'] == 0 and row['uid'] == uid and row['gid'] == gid and row['mode'] == mode and
                         row['label_hex'] == ('u:object_r:' + label + ':s0\0').encode().hex())
    complete = all(value is not None for value in matched.values())
    result.update(directory_contract=matched, directories_match_original_rc=all(matched.values()) if complete else None,
                  attributes_denied=all(row['stat_result'] == row['label_result'] == -13 for row in attributes))
    return result
