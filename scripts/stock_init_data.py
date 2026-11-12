"""检查原 init 的 ext4 数据盘、同步记录与恢复后的 SQLite。"""
import hashlib
import json
from datetime import datetime
from pathlib import Path
import re
import sqlite3
import struct
import subprocess
from tempfile import TemporaryDirectory

DEBUGFS = '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'
SQLITE_NAMES = tuple('persistent.sqlite' + suffix for suffix in ('', '-wal', '-journal', '-shm'))


def inspect(path):
    with path.open('rb') as disk:
        disk.seek(1024)
        block = disk.read(1024)
        disk.seek(0)
        digest = hashlib.file_digest(disk, 'sha256').hexdigest()
    if len(block) != 1024 or struct.unpack_from('<H', block, 0x38)[0] != 0xef53:
        raise ValueError('数据盘需要 ext4：' + str(path))
    incompat, ro_compat = struct.unpack_from('<II', block, 0x60)
    blocks = struct.unpack_from('<I', block, 4)[0]
    reserved = struct.unpack_from('<I', block, 8)[0]
    if incompat & 0x80:
        blocks |= struct.unpack_from('<I', block, 0x150)[0] << 32
        reserved |= struct.unpack_from('<I', block, 0x154)[0] << 32
    block_shift = struct.unpack_from('<I', block, 0x18)[0]
    if block_shift > 6:
        raise ValueError('ext4 block size 无效：' + str(path))
    block_bytes = 1024 << block_shift
    state = struct.unpack_from('<H', block, 0x3a)[0]
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest,
            'block_bytes': block_bytes, 'filesystem_bytes': blocks * block_bytes,
            'reserved_blocks': reserved, 'reserved_uid': struct.unpack_from('<H', block, 0x50)[0],
            'reserved_gid': struct.unpack_from('<H', block, 0x52)[0], 'state': state,
            'needs_recovery': bool(incompat & 4), 'orphan_present': bool(ro_compat & 0x10000),
            'clean': state == 1 and not incompat & 4 and not ro_compat & 0x10000,
            'uuid': block[0x68:0x78].hex()}


def validate(rows, enabled, mode):
    kinds = ('stock_init_data_mount', 'stock_init_data_open', 'stock_init_data_retained', 'stock_init_data_generation', 'stock_init_data_cleanup')
    groups = [[row for row in rows if kind in row] for kind in kinds]
    if not enabled:
        return not any(groups)
    if any(len(group) != 1 for group in groups):
        return False
    mount, opened, held, generation, cleanup = [group[0] for group in groups]
    disks = [row for row in rows if 'host_disk_close' in row]
    contexts = [row for row in rows if row.get('test') == 'translated_context_cleanup']
    shutdown = [row for row in rows if 'stock_init_shutdown' in row]
    expected_disks = [{'host_disk_close': True, 'slot': slot, 'sync_attempted': slot == 0,
                       'sync_result': 0, 'close_result': 0} for slot in (0, 1)]
    if (disks != expected_disks or any(row['host_disk_close'] is not True or
                                     type(row['sync_attempted']) is not bool or
                                     any(type(row[key]) is not int for key in ('slot', 'sync_result', 'close_result')) for row in disks) or
            len(contexts) != 1 or len(shutdown) != 1):
        return False
    ordered = (mount, opened, held, generation, cleanup, *disks, contexts[0], shutdown[0])
    positions = [rows.index(row) for row in ordered]
    if any(before >= after for before, after in zip(positions, positions[1:])):
        return False
    sdk_cleanup = [row for row in rows if 'stock_init_sdk_dm_removed' in row or 'stock_init_sdk_cleanup_deferred' in row]
    if any(not positions[4] < rows.index(row) < positions[5] for row in sdk_cleanup):
        return False
    fields = ({'device', 'flags', 'mount_result'}, {'fd', 'flags'}, {'fd', 'device', 'filesystem_type', 'cloexec'},
              {'previous', 'generation', 'device', 'inherited_fd_closed', 'file_fsync', 'directory_fsync'},
              {'held_stat_result', 'path_stat_result', 'held_device', 'path_device', 'sync_result', 'close_result', 'unmount_result'})
    for row, kind, keys in zip((mount, opened, held, generation, cleanup), kinds, fields):
        if set(row) != keys | {kind} or row[kind] is not True or any(type(value) is not int for key, value in row.items()
                                     if key not in (kind, 'cloexec', 'inherited_fd_closed')):
            return False
    return (mount['mount_result'] == 0 and mount['flags'] == 1030 and mount['device'] > 0 and
            opened['fd'] == held['fd'] >= 3 and opened['flags'] == 0x84000 and
            held['cloexec'] is True and held['filesystem_type'] == 0xef53 and
            held['device'] == generation['device'] == cleanup['held_device'] == cleanup['path_device'] == mount['device'] and
            0 <= generation['previous'] < (1 << 64) - 1 and generation['generation'] == generation['previous'] + 1 and
            generation['inherited_fd_closed'] is True and
            generation['file_fsync'] == generation['directory_fsync'] == 0 and
            cleanup['held_stat_result'] == cleanup['path_stat_result'] == cleanup['sync_result'] == cleanup['close_result'] == 0 and
            cleanup['unmount_result'] in ((0,) if mode == 'lifecycle' else (0, -1, -13)))


def _sqlite_files(directory):
    files = {}
    for name in SQLITE_NAMES:
        path = directory / name
        files[name] = {'present': path.exists()}
        if path.exists():
            with path.open('rb') as stream:
                files[name].update(bytes=path.stat().st_size, sha256=hashlib.file_digest(stream, 'sha256').hexdigest())
    return files


def _query_sqlite(out):
    # mode=ro 可以修改或创建 SHM；原导出字节保留，查询使用独立 APFS 副本。
    original = _sqlite_files(out)
    result = {'sqlite_header': (out / 'persistent.sqlite').read_bytes()[:16].hex()}
    manifest = {'mode': 'ro', 'sqlite_version': sqlite3.sqlite_version,
                'scope': '文件格式、quick_check 与 schema；存在的 WAL、journal、SHM 随数据库共同查询。',
                'original_before': original, 'clones': []}
    root = Path(__file__).resolve().parents[1]
    log_directory = root / ('work/cleanup-' + datetime.now().strftime('%Y-%m-%d'))
    log_directory.mkdir(exist_ok=True)
    key = hashlib.sha256(str(out.resolve()).encode()).hexdigest()[:12]
    log = log_directory / (out.parent.name + '-sqlite-query-' + key + '.json')
    with log.open('x') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    query = None
    try:
        with TemporaryDirectory(prefix='sqlite-query-', dir=out) as directory:
            query = Path(directory)
            manifest['directory'] = str(query)
            try:
                for name, record in original.items():
                    if record['present']:
                        command = ['cp', '-c', str(out / name), str(query / name)]
                        clone = {'command': command, 'source': record}
                        manifest['clones'].append(clone)
                        clone['returncode'] = subprocess.run(command, check=True, timeout=30).returncode
                manifest['before'] = _sqlite_files(query)
                if manifest['before'] != original:
                    raise ValueError('SQLite 查询副本摘要不匹配')
                connection = sqlite3.connect((query / 'persistent.sqlite').as_uri() + '?mode=ro', uri=True)
                try:
                    result['quick_check'] = [row[0] for row in connection.execute('PRAGMA quick_check')]
                    result['schema'] = list(connection.execute('SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY type,name'))
                finally:
                    connection.close()
                    manifest['connection_closed'] = True
            except sqlite3.Error as error:
                result['sqlite_error'] = str(error)
            finally:
                manifest['after'] = _sqlite_files(query)
                manifest['deleted_files'] = [{'path': str(query / name), **record}
                                             for name, record in manifest['after'].items() if record['present']]
                manifest['original_after'] = _sqlite_files(out)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        manifest['error'] = str(error)
        raise
    finally:
        manifest['cleanup_completed'] = query is None or not query.exists()
        manifest['deleted_directories'] = [str(query)] if query is not None and not query.exists() else []
        log.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    if manifest['original_after'] != original:
        raise ValueError('SQLite 查询期间原导出文件变化')
    with log.open('rb') as stream:
        manifest['cleanup_manifest'] = {'path': str(log), 'sha256': hashlib.file_digest(stream, 'sha256').hexdigest()}
    result['sqlite_query'] = manifest
    return result


def export_clean(path, out):
    state = inspect(path)
    if not state['clean']:
        raise ValueError('数据盘尚需 Linux 挂载恢复：' + str(path))
    out.mkdir()
    command = [DEBUGFS, '-R', 'cat /.nce-init-generation', str(path)]
    process = subprocess.run(command, capture_output=True, text=True, check=True, timeout=30)
    match = re.fullmatch(r'DroidOnDarwin stock-init generation (\d+)\n', process.stdout)
    if not match:
        raise ValueError('恢复后的 generation 文件无效')
    files = {}
    commands = [command]
    for name in SQLITE_NAMES:
        target = out / name
        command = [DEBUGFS, '-R', f'dump /misc/keystore/{name} {target}', str(path)]
        process = subprocess.run(command, capture_output=True, text=True, check=True, timeout=30)
        commands.append(command)
        files[name] = {'stderr': process.stderr}
    for name, record in _sqlite_files(out).items():
        files[name].update(record)
    result = {'filesystem': state, 'generation': int(match[1]), 'files': files, 'commands': commands}
    database = out / 'persistent.sqlite'
    if database.exists():
        result.update(_query_sqlite(out))
    return result
