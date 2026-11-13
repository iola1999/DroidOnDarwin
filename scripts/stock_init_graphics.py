"""在固定无加密 SDK 系统盘中替换 allocator、mapper 和 qemupipe。"""
import argparse
from collections import defaultdict
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess

from prepare_android_runtime import partition_extents
from stock_init_metadata import read_partition_metadata
from stock_init_sdk import PARTITIONS
import stock_init_plain_data as plain

PLAIN_SYSTEM_SHA = '2044b2871cea537e570a6c2b8d6ac6bcff5e2722cd318502ecdfa0ed453f0021'
PLAIN_VENDOR_SHA = '6b3dd4072e03981b9ead442ea614c0a9d968c22e9d2ccb7c0af5067eb4ec694d'
FILES = {
    'allocator': ('/bin/hw/android.hardware.graphics.allocator-service.ranchu', 52904, '79351e721a7b169cd5b967c3a29a1aee13b9b2e6b267ad0c621aa553f175ea0d'),
    'mapper': ('/lib64/hw/mapper.ranchu.so', 69096, '311ccf0653ab04154fa3c6acbfa99b81f0dc904f501228dee4dc8725e0a6b870'),
    'qemupipe': ('/lib64/libqemupipe.ranchu.so', 51320, 'bf798c9c9eb9972533260cf47c95df7f3c37f1bac0e6039da4d6bf6466f74169'),
}
VENDOR_OFFSET, VENDOR_BYTES, _ = PARTITIONS['vendor']
E2FSCK = Path('/opt/homebrew/opt/e2fsprogs/sbin/e2fsck')


def _library():
    lib, block_callback = plain._library()
    pointer = c.c_void_p
    attr_callback = c.CFUNCTYPE(c.c_int, c.c_char_p, pointer, c.c_size_t, pointer)
    declarations = {
        'ext2fs_read_bitmaps': [pointer],
        'ext2fs_xattrs_open': [pointer, c.c_uint, c.POINTER(pointer)],
        'ext2fs_xattrs_read': [pointer],
        'ext2fs_xattrs_iterate': [pointer, attr_callback, pointer],
        'ext2fs_xattrs_close': [c.POINTER(pointer)],
    }
    for name, arguments in declarations.items():
        function = getattr(lib, name)
        function.argtypes, function.restype = arguments, c.c_long
    return lib, block_callback, attr_callback


def _attributes(lib, callback_type, fs, ino):
    values, handle = {}, c.c_void_p()

    @callback_type
    def visit(name, value, size, private):
        values[name.decode('ascii')] = c.string_at(value, size).hex()
        return 0

    plain._check(lib.ext2fs_xattrs_open(fs, ino, c.byref(handle)), 'xattrs_open')
    try:
        plain._check(lib.ext2fs_xattrs_read(handle), 'xattrs_read')
        plain._check(lib.ext2fs_xattrs_iterate(handle, visit, None), 'xattrs_iterate')
    finally:
        plain._check(lib.ext2fs_xattrs_close(c.byref(handle)), 'xattrs_close')
    return values


def _file_hash(lib, fs, ino):
    handle, digest = c.c_void_p(), hashlib.sha256()
    plain._check(lib.ext2fs_file_open(fs, ino, 0, c.byref(handle)), 'file_open_read')
    try:
        buffer, got = c.create_string_buffer(1 << 20), c.c_uint()
        while True:
            plain._check(lib.ext2fs_file_read(handle, buffer, len(buffer), c.byref(got)), 'file_read')
            if not got.value:
                break
            digest.update(buffer.raw[:got.value])
    finally:
        plain._check(lib.ext2fs_file_close(handle), 'file_close_read')
    return digest.hexdigest()


def _state(image):
    lib, block_callback, attr_callback = _library()
    rows, references = {}, defaultdict(list)
    with plain._open_vendor(lib, image) as fs:
        scan, current = c.c_void_p(), c.c_uint()

        @block_callback
        def visit(_fs, block, logical, reference, offset, private):
            if block[0]:
                rows[current.value]['blocks'].append([block[0], logical])
                references[block[0]].append(current.value)
            return 0

        plain._check(lib.ext2fs_open_inode_scan(fs, 0, c.byref(scan)), 'open_inode_scan')
        try:
            while True:
                inode = c.create_string_buffer(256)
                plain._check(lib.ext2fs_get_next_inode(scan, c.byref(current), inode), 'get_next_inode')
                if not current.value:
                    break
                plain._check(lib.ext2fs_read_inode_full(fs, current, inode, len(inode)), 'read_inode_full')
                mode, links = struct.unpack_from('<H', inode.raw)[0], struct.unpack_from('<H', inode.raw, 26)[0]
                if not mode or not links:
                    continue
                row = {'inode': inode.raw, 'blocks': [], 'xattrs': _attributes(lib, attr_callback, fs, current.value)}
                rows[current.value] = row
                kind = stat.S_IFMT(mode)
                if kind in (stat.S_IFREG, stat.S_IFDIR) or (kind == stat.S_IFLNK and struct.unpack_from('<I', inode.raw, 28)[0]):
                    plain._check(lib.ext2fs_block_iterate3(fs, current.value, 8, None, visit, None), 'block_iterate3')
                if kind == stat.S_IFREG:
                    row['content_sha256'] = _file_hash(lib, fs, current.value)
        finally:
            lib.ext2fs_close_inode_scan(scan)
        files = {}
        for name, (path, _size, _sha) in FILES.items():
            ino = c.c_uint()
            plain._check(lib.ext2fs_namei(fs, 2, 2, path.encode(), c.byref(ino)), 'namei')
            row = rows[ino.value]
            inode = row['inode']
            if stat.S_IFMT(struct.unpack_from('<H', inode)[0]) != stat.S_IFREG or struct.unpack_from('<H', inode, 26)[0] != 1:
                raise ValueError('SDK 图形文件必须为单链接普通文件：' + path)
            for physical, _logical in row['blocks']:
                if references[physical] != [ino.value]:
                    raise ValueError('SDK 图形文件块存在其他引用：' + path)
            files[name] = {'path': path, 'inode': ino.value,
                'bytes': struct.unpack_from('<I', inode, 4)[0] | struct.unpack_from('<I', inode, 108)[0] << 32,
                'sha256': row['content_sha256'], 'blocks': row['blocks'], 'xattrs': row['xattrs']}
    return rows, files


def _write(image, inputs):
    lib, _block_callback, _attr_callback = _library()
    with plain._open_vendor(lib, image, writable=True) as fs:
        plain._check(lib.ext2fs_read_bitmaps(fs), 'read_bitmaps')
        for name, (path, _size, _sha) in FILES.items():
            ino, handle = c.c_uint(), c.c_void_p()
            plain._check(lib.ext2fs_namei(fs, 2, 2, path.encode(), c.byref(ino)), 'namei_write')
            plain._check(lib.ext2fs_file_open(fs, ino, 1, c.byref(handle)), 'file_open_write')
            try:
                # 原文件所有数据和 extent 块已检查独占，截断不会释放其他文件的共享块。
                plain._check(lib.ext2fs_file_set_size2(handle, 0), 'file_truncate')
                content = inputs[name].read_bytes()
                buffer, written = c.create_string_buffer(content), c.c_uint()
                plain._check(lib.ext2fs_file_write(handle, buffer, len(content), c.byref(written)), 'file_write')
                if written.value != len(content):
                    raise ValueError('SDK 图形文件写入长度不同：' + path)
            finally:
                plain._check(lib.ext2fs_file_close(handle), 'file_close_write')
    with image.open('rb') as stream:
        os.fsync(stream.fileno())


def _inode_metadata_unchanged(before, after):
    normalized = bytearray(after)
    # size、i_blocks、extent 映射和 checksum 随文件重新分配变化，其余 inode 字节保留。
    for start, end in ((4, 8), (28, 32), (40, 100), (108, 112), (116, 118), (124, 126), (130, 132)):
        normalized[start:end] = before[start:end]
    return bytes(normalized) == before


def _vendor_header(image):
    with image.open('rb') as stream:
        stream.seek(VENDOR_OFFSET + 1024)
        superblock = stream.read(1024)
    if len(superblock) != 1024 or struct.unpack_from('<H', superblock, 56)[0] != 0xef53 or struct.unpack_from('<I', superblock, 24)[0] != 2:
        raise ValueError('SDK vendor ext4 超级块不同')
    return {'filesystem_bytes': struct.unpack_from('<I', superblock, 4)[0] * 4096,
        'free_blocks': struct.unpack_from('<I', superblock, 12)[0], 'free_inodes': struct.unpack_from('<I', superblock, 16)[0],
        'feature_compat': struct.unpack_from('<I', superblock, 92)[0],
        'feature_incompat': struct.unpack_from('<I', superblock, 96)[0],
        'feature_ro_compat': struct.unpack_from('<I', superblock, 100)[0]}


def _filesystem_check(candidate, output):
    partition, log = output / 'vendor-verification.img', output / 'vendor-e2fsck.log'
    if partition.exists() or partition.is_symlink():
        raise ValueError('SDK vendor 校验中间文件已存在')
    try:
        with candidate.open('rb') as source, partition.open('xb') as target:
            source.seek(VENDOR_OFFSET)
            remaining = VENDOR_BYTES
            while remaining:
                block = source.read(min(remaining, 4 << 20))
                if not block:
                    raise ValueError('SDK vendor 校验分区截断')
                target.write(block)
                remaining -= len(block)
        before = plain._sha(partition)
        command = [str(E2FSCK), '-f', '-n', str(partition)]
        with log.open('w') as stream:
            run = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=60)
        after = plain._sha(partition)
        result = {'command': command, 'returncode': run.returncode, 'log': str(log), 'log_sha256': plain._sha(log),
            'partition_sha256_before': before, 'partition_sha256_after': after,
            'removed_partition': {'path': str(partition), 'bytes': partition.stat().st_size, 'sha256': after}}
        return result
    finally:
        if partition.exists():
            partition.unlink()


def _identity():
    return {
        'source_inputs': {str(Path(__file__).with_name(name)): plain._sha(Path(__file__).with_name(name)) for name in
            ('stock_init_graphics.py', 'stock_init_plain_data.py', 'stock_init_metadata.py', 'stock_init_sdk.py', 'prepare_android_runtime.py')},
        'tool_inputs': {str(path): plain._sha(path) for path in
            (E2FSCK, plain.LIB / 'libext2fs.2.1.dylib', plain.LIB / 'libcom_err.1.1.dylib')},
    }


def read_candidate(report_path):
    """按现行工具来源和实际镜像读回校验已制好的候选；不修改候选或报告。"""
    report_path = Path(report_path).resolve()
    report_sha = plain._sha(report_path)
    report = json.loads(report_path.read_text())
    if (type(report) is not dict or type(report.get('schema')) is not int or report.get('schema') != 1 or
            report.get('status') != 'passed' or report.get('source_unchanged') is not True or
            report.get('source_image', {}).get('sha256') != PLAIN_SYSTEM_SHA or
            report.get('source_whole_sha256_after') != PLAIN_SYSTEM_SHA or set(report.get('inputs', {})) != set(FILES)):
        raise ValueError('SDK 图形候选报告格式或状态不同')
    identity = _identity()
    expected_sources = {Path(path).name: value for path, value in identity['source_inputs'].items()}
    actual_sources = {Path(path).name: value for path, value in report.get('source_inputs', {}).items()}
    if actual_sources != expected_sources or report.get('tool_inputs') != identity['tool_inputs']:
        raise ValueError('SDK 图形候选制镜像源码或工具身份不同')
    source, candidate = Path(report['source_image']['path']).resolve(), Path(report['candidate_image']['path']).resolve()
    if not source.is_file() or plain._sha(source) != PLAIN_SYSTEM_SHA:
        raise ValueError('SDK 图形候选原系统盘摘要不同')
    if (source == candidate or candidate.stat().st_size != report['candidate_image']['bytes'] or
            plain._sha(candidate) != report['candidate_image']['sha256']):
        raise ValueError('SDK 图形候选整盘摘要不同')
    layout = partition_extents(candidate)
    vendor_sha = plain._sha(candidate, VENDOR_OFFSET, VENDOR_BYTES)
    if vendor_sha != report['candidate_vendor_sha256']:
        raise ValueError('SDK 图形候选 vendor 摘要不同')
    for name, (offset, size, expected) in PARTITIONS.items():
        extents = layout['partitions'][name]
        expected = vendor_sha if name == 'vendor' else expected
        if (len(extents) != 1 or extents[0]['offset'] != offset or extents[0]['bytes'] != size or
                plain._sha(candidate, offset, size) != expected):
            raise ValueError('SDK 图形候选 LP 布局或摘要不同：' + name)
    header = _vendor_header(candidate)
    if header != report['header_after'] or _vendor_header(source) != report['header_before']:
        raise ValueError('SDK 图形候选 ext4 参数不同')
    tail = report['avb_tail']
    tail_offset = VENDOR_OFFSET + header['filesystem_bytes']
    tail_bytes = VENDOR_BYTES - header['filesystem_bytes']
    if (tail['offset'] != tail_offset or tail['bytes'] != tail_bytes or plain._sha(candidate, tail_offset, tail_bytes) != tail['sha256'] or
            plain._sha(source, tail_offset, tail_bytes) != tail['sha256']):
        raise ValueError('SDK 图形候选 AVB 尾部不同')
    ranges = [(0, VENDOR_OFFSET), (VENDOR_OFFSET + VENDOR_BYTES, candidate.stat().st_size - VENDOR_OFFSET - VENDOR_BYTES)]
    if len(report['outside_vendor_unchanged']) != len(ranges):
        raise ValueError('SDK 图形候选外围范围数量不同')
    for row, (offset, size) in zip(report['outside_vendor_unchanged'], ranges):
        if (row['offset'] != offset or row['bytes'] != size or plain._sha(candidate, offset, size) != row['sha256'] or
                plain._sha(source, offset, size) != row['sha256']):
            raise ValueError('SDK 图形候选外围内容不同')
    _rows, actual_files = _state(candidate)
    if actual_files != report['files_after']:
        raise ValueError('SDK 图形候选文件、inode、块映射或 xattrs 不同')
    records = {}
    for name, (path, _size, _sha) in FILES.items():
        row = report['inputs'][name]
        if (type(row['bytes']) is not int or row['bytes'] < 64 or type(row['sha256']) is not str or
                len(row['sha256']) != 64 or any(char not in '0123456789abcdef' for char in row['sha256']) or
                actual_files[name]['bytes'] != row['bytes'] or actual_files[name]['sha256'] != row['sha256']):
            raise ValueError('SDK 图形候选输入大小或摘要不同：' + name)
        records[path] = row
    actual_metadata = read_partition_metadata(candidate, records, records, offset=VENDOR_OFFSET)
    if actual_metadata != report['metadata'] or read_partition_metadata(source, {}, records, offset=VENDOR_OFFSET) != actual_metadata:
        raise ValueError('SDK 图形候选所有者、模式、标签或 inode 不同')
    verification = report['filesystem_check']
    if verification['returncode'] != 0 or verification['partition_sha256_before'] != vendor_sha or verification['partition_sha256_after'] != vendor_sha:
        raise ValueError('SDK 图形候选文件系统检查记录不同')
    if plain._sha(report_path) != report_sha:
        raise ValueError('SDK 图形候选报告在读取期间改变')
    return {**report, 'report_path': str(report_path), 'report_sha256': report_sha}


def prepare_system(source_image, output, *, allocator, mapper, qemupipe):
    """克隆固定 plain 系统盘；已有候选只在来源及整盘摘要一致后只读复用。"""
    source_image, output = Path(source_image).resolve(), Path(output).resolve()
    inputs = {name: Path(value).resolve() for name, value in {'allocator': allocator, 'mapper': mapper, 'qemupipe': qemupipe}.items()}
    candidate, report_path = output / 'sdk-graphics-system.img', output / 'sdk-graphics-system.json'
    existing = any(path.exists() or path.is_symlink() for path in (candidate, report_path))
    source_sha = plain._sha(source_image)
    if source_sha != PLAIN_SYSTEM_SHA or plain._sha(source_image, VENDOR_OFFSET, VENDOR_BYTES) != PLAIN_VENDOR_SHA:
        raise ValueError('SDK 图形适配要求固定无加密系统盘')
    layout = partition_extents(source_image)
    for name, (offset, size, expected) in PARTITIONS.items():
        extents = layout['partitions'][name]
        expected = PLAIN_VENDOR_SHA if name == 'vendor' else expected
        if len(extents) != 1 or extents[0]['offset'] != offset or extents[0]['bytes'] != size or plain._sha(source_image, offset, size) != expected:
            raise ValueError('SDK 图形源 LP 布局或摘要不同：' + name)
    before_rows, before_files = _state(source_image)
    for name, (_path, size, expected) in FILES.items():
        if before_files[name]['bytes'] != size or before_files[name]['sha256'] != expected:
            raise ValueError('SDK 原图形文件不同：' + name)
    header_before = _vendor_header(source_image)
    available_blocks = header_before['free_blocks'] + sum(len(row['blocks']) for row in before_files.values())
    required_data_blocks = sum((path.stat().st_size + 4095) // 4096 for path in inputs.values())
    if required_data_blocks > available_blocks:
        raise ValueError('SDK 图形输入的数据块超过可用分配空间')
    input_records = {}
    for name, path in inputs.items():
        content = path.read_bytes()
        if len(content) < 64 or content[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', content, 18)[0] != 183:
            raise ValueError('SDK 图形输入必须为 ARM64 ELF：' + name)
        input_records[name] = {'path': str(path), 'bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest()}
    output.mkdir(parents=True, exist_ok=True)
    if source_image.stat().st_dev != output.stat().st_dev:
        raise ValueError('SDK 图形候选与源镜像需要位于同一文件系统')
    identity = _identity()
    if existing:
        if not candidate.is_file() or candidate.is_symlink() or not report_path.is_file() or report_path.is_symlink():
            raise ValueError('SDK 图形已有候选或报告不完整')
        previous = read_candidate(report_path)
        previous_inputs = {name: {key: row[key] for key in ('bytes', 'sha256')} for name, row in previous.get('inputs', {}).items()}
        current_inputs = {name: {key: row[key] for key in ('bytes', 'sha256')} for name, row in input_records.items()}
        if previous_inputs != current_inputs or Path(previous['candidate_image']['path']).resolve() != candidate:
            raise ValueError('SDK 图形已有候选的来源或整盘摘要不同')
        if plain._sha(source_image) != source_sha:
            raise ValueError('SDK 图形源系统盘在只读复用期间改变')
        return {**previous, 'reused': True}
    report = {'schema': 1, 'status': 'failed', 'source_image': {'path': str(source_image), 'sha256': source_sha},
        'inputs': input_records, 'files_before': before_files, **identity,
        'allocation': {'available_blocks_after_truncate': available_blocks, 'required_data_blocks': required_data_blocks,
            'extent_metadata_check': 'allocation_and_readonly_e2fsck'}}
    try:
        subprocess.run(['cp', '-c', '-n', str(source_image), str(candidate)], capture_output=True, check=True, timeout=60)
        source_stat, target_stat = source_image.stat(), candidate.stat()
        if ((source_stat.st_dev, source_stat.st_ino) == (target_stat.st_dev, target_stat.st_ino) or
                target_stat.st_size != source_stat.st_size or plain._sha(candidate) != source_sha):
            raise ValueError('SDK 图形 APFS 克隆身份或字节不同')
        _write(candidate, inputs)
        after_rows, after_files = _state(candidate)
        changed_inodes = {row['inode'] for row in before_files.values()}
        if set(before_rows) != set(after_rows):
            raise ValueError('SDK 图形候选 inode 集合不同')
        for ino, before in before_rows.items():
            after = after_rows[ino]
            if ino not in changed_inodes and before != after:
                raise ValueError(f'SDK 图形候选修改了其他 inode 或内容：{ino}')
            if ino in changed_inodes and (before['xattrs'] != after['xattrs'] or not _inode_metadata_unchanged(before['inode'], after['inode'])):
                raise ValueError(f'SDK 图形候选 inode 元数据不同：{ino}')
        expected_files = {FILES[name][0]: input_records[name] for name in FILES}
        before_metadata = read_partition_metadata(source_image, {}, expected_files, offset=VENDOR_OFFSET)
        after_metadata = read_partition_metadata(candidate, expected_files, expected_files, offset=VENDOR_OFFSET)
        if before_metadata != after_metadata:
            raise ValueError('SDK 图形候选标签、模式、所有者或 inode 不同')
        for name in FILES:
            if after_files[name]['inode'] != before_files[name]['inode'] or after_files[name]['bytes'] != input_records[name]['bytes'] or after_files[name]['sha256'] != input_records[name]['sha256']:
                raise ValueError('SDK 图形候选文件读回不同：' + name)
        header_after = _vendor_header(candidate)
        for key in header_before:
            if key != 'free_blocks' and header_before[key] != header_after[key]:
                raise ValueError('SDK 图形候选 ext4 格式或 inode 数量不同：' + key)
        tail_offset = VENDOR_OFFSET + header_before['filesystem_bytes']
        tail_bytes = VENDOR_BYTES - header_before['filesystem_bytes']
        tail_sha = plain._sha(source_image, tail_offset, tail_bytes)
        if plain._sha(candidate, tail_offset, tail_bytes) != tail_sha or partition_extents(candidate) != layout:
            raise ValueError('SDK 图形候选 AVB 尾部或 LP 元数据不同')
        outside = []
        for offset, size in ((0, VENDOR_OFFSET), (VENDOR_OFFSET + VENDOR_BYTES, source_stat.st_size - VENDOR_OFFSET - VENDOR_BYTES)):
            expected = plain._sha(source_image, offset, size)
            if plain._sha(candidate, offset, size) != expected:
                raise ValueError('SDK 图形候选修改超出 vendor LP')
            outside.append({'offset': offset, 'bytes': size, 'sha256': expected})
        changes = []
        with source_image.open('rb') as source, candidate.open('rb') as target:
            source.seek(VENDOR_OFFSET)
            target.seek(VENDOR_OFFSET)
            for offset in range(0, VENDOR_BYTES, 4096):
                before, after = source.read(4096), target.read(4096)
                if before != after:
                    changes.append({'offset': VENDOR_OFFSET + offset, 'bytes': 4096, 'source_sha256': hashlib.sha256(before).hexdigest(), 'candidate_sha256': hashlib.sha256(after).hexdigest()})
        report.update(files_after=after_files, metadata=after_metadata, preserved_other_inodes=len(before_rows) - len(changed_inodes),
            header_before=header_before, header_after=header_after, avb_tail={'offset': tail_offset, 'bytes': tail_bytes, 'sha256': tail_sha},
            outside_vendor_unchanged=outside, changed_vendor_blocks=changes, candidate_image={'path': str(candidate), 'bytes': target_stat.st_size, 'sha256': plain._sha(candidate)},
            candidate_vendor_sha256=plain._sha(candidate, VENDOR_OFFSET, VENDOR_BYTES),
            filesystem_check=_filesystem_check(candidate, output))
        verification = report['filesystem_check']
        if verification['returncode'] or verification['partition_sha256_before'] != verification['partition_sha256_after']:
            raise ValueError('SDK vendor 只读文件系统检查失败，结果见 ' + verification['log'])
        for name, path in inputs.items():
            if plain._sha(path) != input_records[name]['sha256']:
                raise ValueError('SDK 图形输入在准备过程中改变：' + name)
        for path, expected in {**identity['source_inputs'], **identity['tool_inputs']}.items():
            if plain._sha(Path(path)) != expected:
                raise ValueError('SDK 图形制镜像源码在准备过程中改变：' + path)
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        report['source_whole_sha256_after'] = plain._sha(source_image)
        report['source_unchanged'] = report['source_whole_sha256_after'] == source_sha
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        if not report['source_unchanged']:
            raise ValueError('SDK 图形原系统盘在准备期间改变')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--allocator', required=True)
    parser.add_argument('--mapper', required=True)
    parser.add_argument('--qemupipe', required=True)
    args = parser.parse_args()
    report = prepare_system(args.image, args.output, allocator=args.allocator, mapper=args.mapper, qemupipe=args.qemupipe)
    print(json.dumps({'status': report['status'], 'candidate_image': report['candidate_image']}))
