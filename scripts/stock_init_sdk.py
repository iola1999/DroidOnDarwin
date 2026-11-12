"""核对 SDK36 固定 LP 原盘，准备标准只读 loop 与原元数据检查。"""
import hashlib
import subprocess

from prepare_android_runtime import partition_extents, system_partition
from stock_init_metadata import read_partition_metadata

PARTITIONS = {
    'system': (3145728, 940838912, '993193cfffd9b1a2ea034bfce5ec18fbb2856e259398ad6c1eb10b689b524e7d'),
    'product': (1196425216, 2345381888, '4fc0f56ff7977cc917a8da51d2c943ebb70426b3a3b912eef884ace3e814c923'),
    'system_ext': (953155584, 242716672, '1323de63e2197fadd226725d98bbf81c9d2ee563820e6d91c56950c4bcac785a'),
    'vendor': (3542089728, 101539840, 'cfc1da4f75e315ac785ebc6e975be3d77a7bd53a5f2569cf44afdb6c1748ea5b'),
    'system_dlkm': (944766976, 7393280, '2e5a72b5d94fd53f2dfcaa8bb75f10f26b0962f0774f49f6aa130b97910b1af1'),
}
SERVICE_FILES = (
    '/vendor/bin/hw/android.hardware.security.keymint-service',
    '/vendor/etc/init/android.hardware.security.keymint-service.rc',
    '/vendor/etc/vintf/manifest.xml',
    '/vendor/etc/vintf/manifest/android.hardware.security.keymint-service.xml',
    '/vendor/etc/vintf/manifest/android.hardware.security.secureclock-service.xml',
    '/vendor/etc/vintf/manifest/android.hardware.security.sharedsecret-service.xml',
)
ROOT_LINKS = {'/adb_keys', '/bin', '/bugreports', '/d', '/etc', '/init', '/sdcard'}


def prepare_partitions(image, records, output):
    layout = partition_extents(image)
    partitions = {}
    with image.open('rb') as source:
        for name, (offset, size, expected) in PARTITIONS.items():
            extents = layout['partitions'][name]
            if len(extents) != 1 or extents[0]['offset'] != offset or extents[0]['bytes'] != size:
                raise ValueError('SDK36 原 LP 单 extent 范围不同：' + name)
            source.seek(offset)
            remaining, digest = size, hashlib.sha256()
            while remaining:
                content = source.read(min(remaining, 4 << 20))
                if not content:
                    raise ValueError('SDK36 原 LP 分区截断：' + name)
                digest.update(content)
                remaining -= len(content)
            if digest.hexdigest() != expected:
                raise ValueError('SDK36 原 LP 分区摘要不同：' + name)
            partitions[name] = {'offset': offset, 'sizelimit': size, 'sha256': expected,
                                'filesystem': 'erofs' if name == 'system_dlkm' else 'ext4', 'readonly': True}

    checked = {*SERVICE_FILES, '/system/bin/init', '/system/bin/keystore2', '/init.environ.rc',
               '/product/etc/selinux/product_sepolicy.cil', '/system_ext/etc/selinux/system_ext_sepolicy.cil',
               '/system_dlkm'}
    expected_metadata, links = {}, {}
    erofs_image = output / 'original-system-dlkm-metadata.erofs'
    for name, partition in partitions.items():
        guest_paths = {path for path in checked if path == '/' + name or path.startswith('/' + name + '/')}
        if name == 'system':
            guest_paths |= {'/init.environ.rc', *ROOT_LINKS}
        paths = {path: (path if name == 'system' else path[len('/' + name):] or '/') for path in guest_paths}
        selected = {raw: records[path] for path, raw in paths.items() if path in records}
        if partition['filesystem'] == 'erofs':
            from hyperos_erofs_metadata import ErofsMetadata
            system_partition(image, erofs_image, name)
            with erofs_image.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != partition['sha256']:
                    raise ValueError('原 EROFS 元数据输入摘要不同')
            reader = ErofsMetadata(erofs_image, lambda command: subprocess.run(command, capture_output=True, timeout=30))
            metadata = {raw: reader.read(raw) for raw in paths.values()}
        else:
            metadata = read_partition_metadata(image, selected, paths.values(),
                                               links=ROOT_LINKS if name == 'system' else (), offset=partition['offset'])
        for path, raw in paths.items():
            item = metadata[raw]
            if path in ROOT_LINKS:
                links[path] = item
                expected_metadata[path] = dict(item, mode=0o777, inode_type=0o120000)
            else:
                expected_metadata[path] = dict(item, inode_type=0o040000 if raw == '/' else 0o100000)
    if links['/etc']['link_target'] != '/system/etc' or links['/init']['link_target'] != '/system/bin/init':
        raise ValueError('SDK36 原根链接目标不同')
    lines = ['%s\t%d\t%d\t%s' % (name, item['offset'], item['sizelimit'], item['filesystem']) for name, item in partitions.items()]
    (output / 'original-partitions.tsv').write_text('\n'.join(lines) + '\n')
    lines = ['%o\t%o\t%d\t%d\t%s\t%s' %
             (item['inode_type'], item['mode'], item['uid'], item['gid'], path, item['selinux_xattr_hex'])
             for path, item in sorted(expected_metadata.items())]
    (output / 'original-metadata.tsv').write_text('\n'.join(lines) + '\n')
    return {'sdk_partitions': partitions, 'lp_metadata': {key: layout[key] for key in
            ('image_bytes', 'super_offset', 'lp_header_sha256', 'lp_tables_sha256')},
            'original_root_links': links, 'expected_runtime_metadata': expected_metadata,
            'metadata_system_dlkm': {'path': str(erofs_image), 'sha256': partitions['system_dlkm']['sha256'],
                                    'bytes': partitions['system_dlkm']['sizelimit']},
            'partition_mounts': [('/' + name, row['filesystem']) for name, row in partitions.items() if name != 'system']}


def validate_loops(rows, partitions, *, mode='lifecycle'):
    loops = [row for row in rows if 'stock_init_sdk_loop' in row]
    released = [row for row in rows if 'stock_init_sdk_loop_released' in row]
    lifecycle = [row for row in rows if 'stock_init_sdk_loop_lifecycle' in row]
    deferred = [row for row in rows if 'stock_init_sdk_cleanup_deferred' in row]
    shutdown = [row for row in rows if 'stock_init_shutdown' in row]
    if not partitions:
        return not loops and not released and not lifecycle and not deferred and not shutdown
    names = list(partitions)
    if deferred:
        expected = {'stock_init_sdk_cleanup_deferred': True, 'path': '/init.environ.rc',
                    'unmount_result': -1, 'closed_loop_fds': len(names)}
        if (mode != 'original' or len(deferred) != 1 or
                deferred[0].get('stock_init_sdk_cleanup_deferred') is not True or
                type(deferred[0].get('unmount_result')) is not int or
                deferred[0]['unmount_result'] not in (-1, -13) or
                type(deferred[0].get('closed_loop_fds')) is not int):
            return False
        expected['unmount_result'] = deferred[0]['unmount_result']
        if deferred != [expected]:
            return False
    expected_shutdown = {'stock_init_shutdown': True, 'halt_result': 0,
                         'host_disks_closed': True, 'kernel_release_result': 0}
    if (shutdown != [expected_shutdown] or
            type(shutdown[0]['halt_result']) is not int or type(shutdown[0]['kernel_release_result']) is not int or
            shutdown[0]['stock_init_shutdown'] is not True or shutdown[0]['host_disks_closed'] is not True):
        return False
    release_names = [] if deferred else names
    if len(loops) != len(names) + 1 or len(released) != len(release_names) + 1 or len(lifecycle) != 1:
        return False
    identities = {}
    for row, (name, temporary) in zip(loops, [('vendor', True), *[(name, False) for name in names]]):
        if (set(row) != {'stock_init_sdk_loop', 'partition', 'loop_id', 'offset', 'sizelimit', 'capacity', 'flags', 'end_read_result', 'temporary'} or
                row['stock_init_sdk_loop'] is not True or row['partition'] != name or row['temporary'] is not temporary or
                any(type(row[key]) is not int for key in ('loop_id', 'offset', 'sizelimit', 'capacity', 'flags', 'end_read_result')) or
                not 0 <= row['loop_id'] < 256 or row['offset'] != partitions[name]['offset'] or
                row['sizelimit'] != partitions[name]['sizelimit'] or row['capacity'] != row['sizelimit'] or
                row['flags'] != 1 or row['end_read_result'] != 0):
            return False
        identities[name, temporary] = row['loop_id']
    if len({identities[name, False] for name in names}) != len(names):
        return False
    for row, (name, temporary) in zip(released, [('vendor', True), *[(name, False) for name in reversed(release_names)]]):
        if (set(row) != {'stock_init_sdk_loop_released', 'partition', 'loop_id', 'clear_result', 'status_result', 'remove_result', 'status_waits', 'remove_waits', 'temporary'} or
                row['stock_init_sdk_loop_released'] is not True or row['partition'] != name or row['temporary'] is not temporary or
                any(type(row[key]) is not int for key in ('loop_id', 'clear_result', 'status_result', 'remove_result', 'status_waits', 'remove_waits')) or
                row['loop_id'] != identities[name, temporary] or row['clear_result'] != 0 or
                row['status_result'] != -6 or row['remove_result'] != 0 or row['status_waits'] < 0 or row['remove_waits'] < 0):
            return False
    row = lifecycle[0]
    return (set(row) == {'stock_init_sdk_loop_lifecycle', 'file_open_result', 'block_open_result', 'block_write_result', 'unmount_result'} and
            row['stock_init_sdk_loop_lifecycle'] is True and
            all(type(row[key]) is int for key in ('file_open_result', 'block_open_result', 'block_write_result', 'unmount_result')) and
            row['file_open_result'] == -30 and row['unmount_result'] == 0 and
            ((row['block_open_result'] >= 0 and row['block_write_result'] == -1) or
             (row['block_open_result'] == -13 and row['block_write_result'] == -13)))
