"""保留固定 HyperOS LZ4 ramdisk 的 newc 文件与原 inode 字段。"""
import hashlib
import subprocess
from pathlib import PurePosixPath

from inspect_hyperos_input import digest, require


def parse_newc(data):
    entries, contents, names = [], {}, set()
    pos = 0
    while True:
        require(data[pos:pos + 6] == b'070701' and pos + 110 <= len(data), '原 ramdisk newc 头无效')
        fields = [int(data[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
        ino, mode, uid, gid, nlink, mtime, size, major, minor, rmajor, rminor, namelen, checksum = fields
        start = pos + 110
        require(namelen > 0 and start + namelen <= len(data) and data[start + namelen - 1] == 0,
                '原 ramdisk newc 名称越界')
        name = data[start:start + namelen - 1].decode('utf-8')
        pos = (start + namelen + 3) & ~3
        require(pos + size <= len(data) and checksum == 0, '原 ramdisk newc 内容越界或 checksum 非零')
        content = data[pos:pos + size]
        pos = (pos + size + 3) & ~3
        if name == 'TRAILER!!!':
            require(size == 0 and not any(data[pos:]), '固定 ramdisk 必须为单个 newc 归档')
            return entries, contents
        source_path = PurePosixPath(name)
        require(name and not source_path.is_absolute() and '..' not in source_path.parts and '\0' not in name,
                '原 ramdisk 包含无效路径')
        path = '/' + str(source_path).removeprefix('./') if name != '.' else '/'
        require(path not in names, '原 ramdisk newc 路径重复：' + path)
        names.add(path)
        entry = {'path': path, 'original_inode': ino, 'mode': mode & 0o7777,
                 'inode_type': mode & 0o170000, 'uid': uid, 'gid': gid, 'nlink': nlink,
                 'mtime': mtime, 'bytes': size, 'dev_major': major, 'dev_minor': minor,
                 'rdev_major': rmajor, 'rdev_minor': rminor,
                 'sha256': hashlib.sha256(content).hexdigest()}
        if entry['inode_type'] == 0o120000:
            entry['link_target'] = content.decode('utf-8')
        entries.append(entry)
        contents[path] = content


def extract_ramdisk(image, output):
    require(image.read_bytes()[:4] == b'\x02\x21\x4c\x18', '固定 ramdisk 必须为 LZ4 legacy')
    result = subprocess.run(['/opt/homebrew/bin/lz4', '-d', '-c', str(image)],
                            capture_output=True, check=True, timeout=30)
    cpio = output / 'ramdisk.cpio'
    cpio.write_bytes(result.stdout)
    entries, contents = parse_newc(result.stdout)
    for entry in entries:
        if entry['inode_type'] == 0o100000:
            target = output / 'ramdisk-root' / entry['path'].lstrip('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents[entry['path']])
            target.chmod(entry['mode'])
            entry['extracted'] = str(target)
    return {'image': str(image), 'image_bytes': image.stat().st_size, 'image_sha256': digest(image),
            'compression': 'lz4-legacy', 'archive_count': 1, 'cpio_bytes': cpio.stat().st_size,
            'cpio_sha256': digest(cpio), 'entries': entries}
