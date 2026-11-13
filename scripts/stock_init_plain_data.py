"""在独立 SDK36 系统盘候选中准备无加密的 /data fstab。"""
import copy
import ctypes as c
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess

from prepare_android_runtime import partition_extents
from stock_init_metadata import LIB, read_partition_metadata
from stock_init_sdk import PARTITIONS
from stock_init_sdk_ramdisk import read as read_ramdisk

SYSTEM_SHA = '54a96f82a138e0919686cefffead8861e10c6189bf94188fef4fa08e24dc3ed1'
FSTAB_SHA = 'b4572d0245a6bd33fec9b3fcee224a314e4ad62d44dd4f88d636983e6d47b144'
FSTAB_PATH = '/etc/fstab.ranchu'
REMOVED_OPTIONS = (b'fileencryption=aes-256-xts:aes-256-cts', b'keydirectory=/metadata/vold/metadata_encryption')
VENDOR_OFFSET, VENDOR_BYTES, VENDOR_SHA = PARTITIONS['vendor']


def transform_fstab(content):
    """只接受固定原始 fstab，保留指定两项选项以外的全部字节。"""
    if type(content) is not bytes or hashlib.sha256(content).hexdigest() != FSTAB_SHA:
        raise ValueError('SDK36 原 fstab 摘要不同')
    lines = content.splitlines(keepends=True)
    indexes = [index for index, line in enumerate(lines) if len(line.split()) == 5 and line.split()[1] == b'/data']
    if indexes != [14]:
        raise ValueError('SDK36 原 /data fstab 位置不同')
    line = lines[14]
    for option in REMOVED_OPTIONS:
        token = b',' + option
        if line.count(token) != 1:
            raise ValueError('SDK36 原 /data fstab 选项不同')
        line = line.replace(token, b'')
    lines[14] = line
    return b''.join(lines)


def _sha(path, offset=0, size=None):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        stream.seek(offset)
        remaining = path.stat().st_size - offset if size is None else size
        while remaining:
            block = stream.read(min(remaining, 4 << 20))
            if not block:
                raise ValueError('SDK36 系统镜像范围截断')
            digest.update(block)
            remaining -= len(block)
    return digest.hexdigest()


def _check(result, operation):
    if result:
        raise ValueError(f'SDK36 vendor fstab 操作失败：{operation}，错误码 {result}')


def _library():
    c.CDLL(str(LIB / 'libcom_err.1.1.dylib'), mode=c.RTLD_GLOBAL)
    lib = c.CDLL(str(LIB / 'libext2fs.2.1.dylib'))
    pointer, ino, u64 = c.c_void_p, c.c_uint, c.c_uint64
    callback = c.CFUNCTYPE(c.c_int, pointer, c.POINTER(u64), c.c_int64, u64, c.c_int, pointer)
    declarations = {
        'ext2fs_open2': [c.c_char_p, c.c_char_p, c.c_int, c.c_int, c.c_uint, pointer, c.POINTER(pointer)],
        'ext2fs_close': [pointer],
        'ext2fs_namei': [pointer, ino, ino, c.c_char_p, c.POINTER(ino)],
        'ext2fs_read_inode_full': [pointer, ino, pointer, c.c_int],
        'ext2fs_file_open': [pointer, ino, c.c_int, c.POINTER(pointer)],
        'ext2fs_file_read': [pointer, pointer, c.c_uint, c.POINTER(c.c_uint)],
        'ext2fs_file_write': [pointer, pointer, c.c_uint, c.POINTER(c.c_uint)],
        'ext2fs_file_set_size2': [pointer, c.c_int64],
        'ext2fs_file_close': [pointer],
        'ext2fs_bmap2': [pointer, ino, pointer, pointer, c.c_int, u64, c.POINTER(c.c_int), c.POINTER(u64)],
        'ext2fs_open_inode_scan': [pointer, c.c_int, c.POINTER(pointer)],
        'ext2fs_get_next_inode': [pointer, c.POINTER(ino), pointer],
        'ext2fs_block_iterate3': [pointer, ino, c.c_int, pointer, callback, pointer],
    }
    for name, arguments in declarations.items():
        function = getattr(lib, name)
        function.argtypes = arguments
        function.restype = c.c_long
    lib.ext2fs_close_inode_scan.argtypes = [pointer]
    lib.ext2fs_close_inode_scan.restype = None
    lib.ext2fs_inode_table_loc.argtypes = [pointer, c.c_uint]
    lib.ext2fs_inode_table_loc.restype = u64
    return lib, callback


@contextmanager
def _open_vendor(lib, image, *, writable=False):
    fs = c.c_void_p()
    # ext2fs.h 声明 EXT2_FLAG_RW=1；原盘只按 flags=0 打开。
    _check(lib.ext2fs_open2(str(image).encode(), f'offset={VENDOR_OFFSET}'.encode(), int(writable), 0, 0,
                           c.c_void_p.in_dll(lib, 'unix_io_manager'), c.byref(fs)), 'open')
    try:
        yield fs
    finally:
        _check(lib.ext2fs_close(fs), 'close')


def _read_fstab(lib, fs):
    ino = c.c_uint()
    _check(lib.ext2fs_namei(fs, 2, 2, FSTAB_PATH.encode(), c.byref(ino)), 'namei')
    raw = c.create_string_buffer(256)
    _check(lib.ext2fs_read_inode_full(fs, ino, raw, len(raw)), 'read_inode_full')
    mode, size = struct.unpack_from('<H', raw.raw)[0], struct.unpack_from('<I', raw.raw, 4)[0]
    if (stat.S_IFMT(mode) != stat.S_IFREG or not 0 < size < 4096 or
            struct.unpack_from('<I', raw.raw, 108)[0] or struct.unpack_from('<H', raw.raw, 26)[0] != 1):
        raise ValueError('SDK36 vendor fstab 必须为单链接且小于一个块的普通文件')
    file = c.c_void_p()
    _check(lib.ext2fs_file_open(fs, ino, 0, c.byref(file)), 'file_open_read')
    try:
        buffer, got = c.create_string_buffer(4096), c.c_uint()
        _check(lib.ext2fs_file_read(file, buffer, len(buffer), c.byref(got)), 'file_read')
        if got.value != size:
            raise ValueError('SDK36 vendor fstab 读取长度不同')
        content = buffer.raw[:got.value]
    finally:
        _check(lib.ext2fs_file_close(file), 'file_close_read')
    return ino.value, raw.raw, content


def _exclusive_block(lib, callback_type, fs, ino, raw):
    physical, flags = c.c_uint64(), c.c_int()
    inode_buffer = c.create_string_buffer(raw)
    _check(lib.ext2fs_bmap2(fs, ino, inode_buffer, None, 0, 0, c.byref(flags), c.byref(physical)), 'bmap2')
    if not physical.value or flags.value:
        raise ValueError('SDK36 vendor fstab 数据块映射不同')
    scan, current = c.c_void_p(), c.c_uint()
    references = []

    @callback_type
    def visit(_fs, block, _logical, _reference, _offset, _private):
        if block[0] == physical.value:
            references.append(current.value)
            return 2  # ext2fs.h 的 BLOCK_ABORT，仅结束当前 inode 的只读遍历。
        return 0

    _check(lib.ext2fs_open_inode_scan(fs, 0, c.byref(scan)), 'open_inode_scan')
    scanned = 0
    try:
        node = c.create_string_buffer(128)
        while True:
            _check(lib.ext2fs_get_next_inode(scan, c.byref(current), node), 'get_next_inode')
            if not current.value:
                break
            mode = struct.unpack_from('<H', node.raw)[0]
            links, blocks = struct.unpack_from('<H', node.raw, 26)[0], struct.unpack_from('<I', node.raw, 28)[0]
            if links and (stat.S_IFMT(mode) in (stat.S_IFREG, stat.S_IFDIR) or
                          (stat.S_IFMT(mode) == stat.S_IFLNK and blocks)):
                scanned += 1
                _check(lib.ext2fs_block_iterate3(fs, current, 8, None, visit, None), 'block_iterate3')
    finally:
        lib.ext2fs_close_inode_scan(scan)
    return {'physical_block': physical.value, 'references': references, 'scanned_inodes': scanned}


def _read_vendor(image):
    with image.open('rb') as source:
        source.seek(VENDOR_OFFSET + 1024)
        superblock = source.read(1024)
    if (len(superblock) != 1024 or struct.unpack_from('<H', superblock, 56)[0] != 0xef53 or
            struct.unpack_from('<I', superblock, 24)[0] != 2 or struct.unpack_from('<H', superblock, 88)[0] != 256):
        raise ValueError('SDK36 vendor 的 ext4 块大小或 inode 大小不同')
    lib, callback = _library()
    with _open_vendor(lib, image) as fs:
        ino, inode, content = _read_fstab(lib, fs)
        exclusive = _exclusive_block(lib, callback, fs, ino, inode)
        inodes_per_group = struct.unpack_from('<I', superblock, 40)[0]
        if not inodes_per_group:
            raise ValueError('SDK36 vendor 的 inode 分组大小无效')
        group, index = divmod(ino - 1, inodes_per_group)
        table = lib.ext2fs_inode_table_loc(fs, group)
        inode_offset = VENDOR_OFFSET + table * 4096 + index * 256
    with image.open('rb') as source:
        source.seek(inode_offset)
        if source.read(256) != inode:
            raise ValueError('SDK36 vendor fstab 的 inode 位置校验失败')
    return {'inode': ino, 'inode_bytes': inode, 'content': content, 'exclusive_data_block': exclusive,
            'inode_offset': inode_offset, 'shared_blocks_feature': bool(struct.unpack_from('<I', superblock, 100)[0] & 0x4000)}


def _write_vendor(candidate, content):
    lib, callback = _library()
    with _open_vendor(lib, candidate, writable=True) as fs:
        ino, inode, original = _read_fstab(lib, fs)
        if hashlib.sha256(original).hexdigest() != FSTAB_SHA or content != transform_fstab(original):
            raise ValueError('SDK36 候选 vendor fstab 写入前的字节不同')
        if _exclusive_block(lib, callback, fs, ino, inode)['references'] != [ino]:
            raise ValueError('SDK36 vendor fstab 数据块存在其他 inode 引用')
        file = c.c_void_p()
        # EXT2_FILE_WRITE=1。新内容仍使用原数据块，先清除原文件尾部再截断。
        _check(lib.ext2fs_file_open(fs, ino, 1, c.byref(file)), 'file_open_write')
        try:
            padded = content + bytes(len(original) - len(content))
            buffer, written = c.create_string_buffer(padded), c.c_uint()
            _check(lib.ext2fs_file_write(file, buffer, len(padded), c.byref(written)), 'file_write')
            if written.value != len(padded):
                raise ValueError('SDK36 候选 vendor fstab 写入长度不同')
            _check(lib.ext2fs_file_set_size2(file, len(content)), 'file_set_size2')
        finally:
            _check(lib.ext2fs_file_close(file), 'file_close_write')
    with candidate.open('rb') as stream:
        # libext2fs close 已完成同步；再次同步宿主 fd 后才开始读回。
        os.fsync(stream.fileno())


def _inode_unchanged(before, after):
    # ext2_fs.h 的 size 与 checksum 字段随内容长度变化，其余 inode 字节必须保留。
    normalized = bytearray(after)
    for start, end in ((4, 8), (108, 112), (124, 126), (130, 132)):
        normalized[start:end] = before[start:end]
    return bytes(normalized) == before


def _changed_vendor_blocks(original, candidate, source_fstab):
    changes = []
    inode_offset = source_fstab['inode_offset']
    data_offset = VENDOR_OFFSET + source_fstab['exclusive_data_block']['physical_block'] * 4096
    allowed = [(data_offset, data_offset + len(source_fstab['content']))]
    allowed += [(inode_offset + start, inode_offset + end) for start, end in ((4, 8), (108, 112), (124, 126), (130, 132))]
    with original.open('rb') as source, candidate.open('rb') as target:
        source.seek(VENDOR_OFFSET)
        target.seek(VENDOR_OFFSET)
        for relative in range(0, VENDOR_BYTES, 4096):
            before, after = source.read(4096), target.read(4096)
            if len(before) != 4096 or len(after) != 4096:
                raise ValueError('SDK36 vendor 候选比较范围截断')
            if before != after:
                for index, (a, b) in enumerate(zip(before, after)):
                    absolute = VENDOR_OFFSET + relative + index
                    if a != b and not any(start <= absolute < end for start, end in allowed):
                        raise ValueError(f'libext2fs 修改超出 fstab 内容与 inode 长度、校验字段：偏移 {absolute}')
                changes.append({'offset': VENDOR_OFFSET + relative, 'bytes': 4096,
                                'changed_bytes': sum(a != b for a, b in zip(before, after)),
                                'source_sha256': hashlib.sha256(before).hexdigest(),
                                'candidate_sha256': hashlib.sha256(after).hexdigest()})
    return changes


def prepare_system(original_image, output, mounted_input):
    """接收原 prepare() 结果，克隆并校验候选；返回值明确区分来源与实际部署。"""
    original_image, output = Path(original_image).resolve(), Path(output).resolve()
    candidate, report_path = output / 'sdk-plain-system.img', output / 'sdk-plain-system.json'
    check_path = output / 'sdk-plain-source-check.json'
    if any(path.exists() or path.is_symlink() for path in (candidate, report_path, check_path)):
        raise ValueError('SDK36 无加密候选输出已存在')
    source_sha = _sha(original_image)
    if source_sha != SYSTEM_SHA:
        raise ValueError('SDK36 原系统整盘摘要不同')
    for name in ('original_system_image', 'deployed_system_partition'):
        record = mounted_input[name]
        if Path(record['path']).resolve() != original_image or record['sha256'] != SYSTEM_SHA:
            raise ValueError('SDK36 无加密候选要求已校验原始系统盘：' + name)
    layout = partition_extents(original_image)
    partitions = mounted_input['sdk_partitions']
    if set(partitions) != set(PARTITIONS):
        raise ValueError('SDK36 原 LP 集合不同')
    for name, (offset, size, expected) in PARTITIONS.items():
        row = partitions[name]
        extents = layout['partitions'][name]
        if (len(extents) != 1 or extents[0]['offset'] != offset or extents[0]['bytes'] != size or
                row['offset'] != offset or row['sizelimit'] != size or row['sha256'] != expected or row['readonly'] is not True or
                _sha(original_image, offset, size) != expected):
            raise ValueError('SDK36 原 LP 范围或摘要不同：' + name)
    source = _read_vendor(original_image)
    content = source['content']
    plain = transform_fstab(content)
    staged = mounted_input['files']['/vendor/etc/fstab.ranchu']
    if (staged['sha256'] != FSTAB_SHA or staged['bytes'] != len(content) or
            Path(staged['source']).read_bytes() != content or Path(staged['staged_path']).read_bytes() != content):
        raise ValueError('SDK36 staged vendor fstab 与原盘字节不同')
    ramdisk = read_ramdisk(original_image.with_name('ramdisk.img'))
    early = [row for row in ramdisk['entries'] if row['path'] == 'first_stage_ramdisk/fstab.ranchu']
    if len(early) != 1 or early[0]['sha256'] != FSTAB_SHA or early[0]['data'] != content:
        raise ValueError('SDK36 原 ramdisk fstab 与 vendor 字节不同')
    original_metadata = read_partition_metadata(original_image, {FSTAB_PATH: {'bytes': len(content), 'sha256': FSTAB_SHA}},
                                                {FSTAB_PATH}, offset=VENDOR_OFFSET)[FSTAB_PATH]
    output.mkdir(parents=True, exist_ok=True)
    if original_image.stat().st_dev != output.stat().st_dev:
        raise ValueError('SDK36 系统候选与原镜像需要位于同一文件系统')
    source_check = {'source_whole_sha256': source_sha, 'source_vendor_sha256': VENDOR_SHA,
                    'source_ramdisk': {key: ramdisk[key] for key in ('input', 'input_sha256')},
                    'source_fstab_sha256': FSTAB_SHA, 'source_metadata': original_metadata,
                    'source_inode_sha256': hashlib.sha256(source['inode_bytes']).hexdigest(),
                    'source_inode_offset': source['inode_offset'],
                    'shared_blocks_feature': source['shared_blocks_feature'],
                    'exclusive_data_block': source['exclusive_data_block']}
    check_path.write_text(json.dumps(source_check, ensure_ascii=False, indent=2) + '\n')
    if source['exclusive_data_block']['references'] != [source['inode']]:
        raise ValueError('SDK36 vendor fstab 数据块存在其他 inode 引用；检查结果已保存')
    try:
        subprocess.run(['cp', '-c', '-n', str(original_image), str(candidate)], capture_output=True, check=True, timeout=60)
        source_stat, candidate_stat = original_image.stat(), candidate.stat()
        if ((source_stat.st_dev, source_stat.st_ino) == (candidate_stat.st_dev, candidate_stat.st_ino) or
                candidate_stat.st_size != source_stat.st_size or _sha(candidate) != SYSTEM_SHA):
            raise ValueError('SDK36 APFS 候选克隆身份或字节不同')
        _write_vendor(candidate, plain)
        actual = _read_vendor(candidate)
        if (actual['content'] != plain or actual['inode'] != source['inode'] or
                actual['exclusive_data_block'] != source['exclusive_data_block'] or
                not _inode_unchanged(source['inode_bytes'], actual['inode_bytes'])):
            raise ValueError('SDK36 候选 fstab 内容或 inode 元数据不同')
        plain_sha = hashlib.sha256(plain).hexdigest()
        actual_metadata = read_partition_metadata(candidate, {FSTAB_PATH: {'bytes': len(plain), 'sha256': plain_sha}},
                                                  {FSTAB_PATH}, offset=VENDOR_OFFSET)[FSTAB_PATH]
        if actual_metadata != original_metadata or partition_extents(candidate) != layout:
            raise ValueError('SDK36 候选 fstab 所有者、标签或 LP 元数据不同')
        lp_results = {}
        for name, (offset, size, expected) in PARTITIONS.items():
            actual_sha = _sha(candidate, offset, size)
            if (actual_sha != expected) != (name == 'vendor'):
                raise ValueError('SDK36 候选 LP 修改范围不同：' + name)
            lp_results[name] = {'source_sha256': expected, 'candidate_sha256': actual_sha, 'changed': name == 'vendor'}
        outside = []
        for offset, size in ((0, VENDOR_OFFSET), (VENDOR_OFFSET + VENDOR_BYTES, source_stat.st_size - VENDOR_OFFSET - VENDOR_BYTES)):
            expected = _sha(original_image, offset, size)
            if _sha(candidate, offset, size) != expected:
                raise ValueError('SDK36 候选修改超出 vendor LP')
            outside.append({'offset': offset, 'bytes': size, 'sha256': expected})
        report = {**source_check, 'data_mode': 'plain',
                  'source_image': {'path': str(original_image), 'sha256': source_sha},
                  'candidate_image': {'path': str(candidate), 'sha256': _sha(candidate), 'bytes': candidate_stat.st_size},
                  'fstab_candidate_sha256': plain_sha,
                  'candidate_vendor_sha256': lp_results['vendor']['candidate_sha256'],
                  'fstab': {'source_sha256': FSTAB_SHA, 'candidate_sha256': plain_sha, 'source_bytes': len(content),
                            'candidate_bytes': len(plain), 'removed_options': [option.decode() for option in REMOVED_OPTIONS],
                            'guest_paths': ['/first_stage_ramdisk/fstab.ranchu', '/vendor/etc/fstab.ranchu'],
                            'candidate_vendor_written': True, 'ramdisk_deployment_pending': True},
                  'candidate_metadata': actual_metadata, 'candidate_inode_sha256': hashlib.sha256(actual['inode_bytes']).hexdigest(),
                  'lp_checks': lp_results, 'outside_vendor_unchanged': outside,
                  'vendor_changed_blocks': _changed_vendor_blocks(original_image, candidate, source),
                  'inode_preserved_except_size_checksum': True, 'metadata_readback_validated': True,
                  'deployment_readonly': True, 'copy': 'cp -c', 'report': str(report_path)}
    finally:
        if _sha(original_image) != source_sha:
            raise ValueError('SDK36 原系统盘在候选准备期间变化')
    report['source_whole_sha256_after'] = source_sha
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    prepared = copy.deepcopy(mounted_input)
    prepared['deployed_system_partition'] = dict(report['candidate_image'])
    prepared['sdk_partitions']['vendor']['sha256'] = report['candidate_vendor_sha256']
    prepared['sdk_plain_data'] = report
    return prepared
