#!/usr/bin/env python3
"""读取固定 SDK36 ramdisk 的两段 newc 记录，文件内容只保留在内存。"""
import hashlib
from pathlib import Path
import re
import stat
import subprocess

RAMDISK_SHA = '5eff5ed46490588b7576716741e19709b8a99dccd5a7f1e7ee1256c027dacf1d'
ARCHIVE_ENTRY_COUNTS = (12, 17)


def _parse(decoded):
    entries, archives = [], []
    offset = 0
    while offset < len(decoded):
        start = offset
        count = 0
        while True:
            if offset + 110 > len(decoded) or decoded[offset:offset + 6] != b'070701':
                raise ValueError('SDK ramdisk newc header 无效或截断')
            header = decoded[offset + 6:offset + 110]
            if re.fullmatch(b'[0-9a-fA-F]{104}', header) is None:
                raise ValueError('SDK ramdisk newc 字段无效')
            fields = [int(header[index * 8:(index + 1) * 8], 16) for index in range(13)]
            ino, mode, uid, gid, nlink, mtime, size, dev_major, dev_minor, rdev_major, rdev_minor, namesize, check = fields
            name_end = offset + 110 + namesize
            data_start = (name_end + 3) & ~3
            data_end = data_start + size
            record_end = (data_end + 3) & ~3
            if namesize < 2 or record_end > len(decoded):
                raise ValueError('SDK ramdisk 文件名、内容或对齐字节截断')
            raw_name = decoded[offset + 110:name_end]
            if raw_name[-1:] != b'\0' or b'\0' in raw_name[:-1]:
                raise ValueError('SDK ramdisk 文件名终止符无效')
            try:
                name = raw_name[:-1].decode('utf-8')
            except UnicodeDecodeError as error:
                raise ValueError('SDK ramdisk 文件名编码无效') from error
            if check or any(decoded[name_end:data_start]) or any(decoded[data_end:record_end]):
                raise ValueError('SDK ramdisk newc check 或对齐字节无效')
            if name == 'TRAILER!!!':
                if size:
                    raise ValueError('SDK ramdisk trailer 包含内容')
                archives.append({'index': len(archives) + 1, 'start_offset': start, 'entry_count': count,
                                 'trailer_offset': offset, 'records_end': record_end})
                offset = record_end
                while offset < len(decoded) and decoded[offset] == 0:
                    offset += 1
                archives[-1]['padding_end'] = offset
                break
            if name.startswith('/') or any(part in ('', '.', '..') for part in name.split('/')):
                raise ValueError('SDK ramdisk 路径无法用于目标 root')
            inode_type = stat.S_IFMT(mode)
            if inode_type not in (stat.S_IFDIR, stat.S_IFCHR, stat.S_IFREG):
                raise ValueError('SDK ramdisk 记录类型超出固定输入范围')
            if inode_type != stat.S_IFREG and size:
                raise ValueError('SDK ramdisk 目录或节点包含文件内容')
            data = decoded[data_start:data_end]
            entries.append({'archive': len(archives) + 1, 'path': name, 'inode': ino, 'mode': mode,
                            'inode_type': inode_type, 'uid': uid, 'gid': gid, 'nlink': nlink, 'mtime': mtime,
                            'device_major': dev_major, 'device_minor': dev_minor,
                            'rdev_major': rdev_major, 'rdev_minor': rdev_minor,
                            'header_offset': offset, 'data_offset': data_start, 'record_end': record_end,
                            'bytes': size, 'sha256': hashlib.sha256(data).hexdigest(), 'data': data})
            count += 1
            offset = record_end
    # 固定输入的第二段包含 first-stage fstab；只到首个 trailer 的读取必须失败。
    if tuple(archive['entry_count'] for archive in archives) != ARCHIVE_ENTRY_COUNTS:
        raise ValueError('固定 SDK ramdisk 需要完整的 12、17 条两段 archive')
    return archives, entries


def read(path):
    """返回原 stream 顺序的元数据和 data: bytes，文件内容仅在内存保存。"""
    path = Path(path).resolve()
    packed = path.read_bytes()
    digest = hashlib.sha256(packed).hexdigest()
    if digest != RAMDISK_SHA:
        raise ValueError('SDK36 原 ramdisk 摘要不同')
    process = subprocess.run(['lz4', '-d', '-c', '-'], input=packed, capture_output=True, timeout=30, check=True)
    decoded = process.stdout
    archives, entries = _parse(decoded)
    return {'input': str(path), 'input_bytes': len(packed), 'input_sha256': digest,
            'compression': 'LZ4 legacy frame', 'decoded_bytes': len(decoded),
            'decoded_sha256': hashlib.sha256(decoded).hexdigest(), 'archives': archives, 'entries': entries}
