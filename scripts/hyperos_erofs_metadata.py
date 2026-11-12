"""读取固定 HyperOS vendor/APEX 的 EROFS inode 和原 SELinux xattr。"""
import re
import struct
from pathlib import Path

from inspect_hyperos_input import require

DUMP = Path('/opt/homebrew/bin/dump.erofs')


class ErofsMetadata:
    def __init__(self, image, run):
        self.image = image
        self.run = run
        self.data = image.read_bytes()
        sb = self.data[1024:1152]
        require(len(sb) == 128 and struct.unpack_from('<I', sb)[0] == 0xe0f5e1e2,
                '需要原 EROFS 分区')
        require(sb[12] == 12 and sb[91] == 0, '固定输入只接受 4 KiB EROFS 与普通 xattr 名称')
        self.meta, self.xattr = struct.unpack_from('<II', sb, 40)

    def read(self, path):
        result = self.run([DUMP, '--path=' + path, self.image])
        text = result.stdout.decode()
        match = re.search(r'NID: (\d+)', text)
        if not match:
            require(result.stderr == ('<E> erofs: read inode failed @ ' + path + '\n').encode(),
                    'EROFS 路径检查失败：' + path)
            return None
        require(result.returncode == 0, 'EROFS inode 检查失败：' + path)
        nid = int(match[1])
        offset = self.meta * 4096 + nid * 32
        require(offset + 64 <= len(self.data), 'EROFS inode 越界')
        version, count, mode = struct.unpack_from('<HHH', self.data, offset)
        inode_size = 64 if version & 1 else 32
        uid, gid = struct.unpack_from('<II' if inode_size == 64 else '<HH', self.data, offset + 24)
        native = re.search(r'Uid: (\d+)\s+Gid: (\d+)\s+Access: ([0-7]+)', text)
        require(native is not None and (uid, gid, mode & 0o7777) ==
                (int(native[1]), int(native[2]), int(native[3], 8)),
                'EROFS inode 与 dump.erofs 所有者或模式不同：' + path)
        attrs = {}

        def entry(at):
            require(at + 4 <= len(self.data), 'EROFS xattr 头越界')
            namelen, index, valuelen = struct.unpack_from('<BBH', self.data, at)
            end = at + 4 + namelen + valuelen
            require(not index & 128 and end <= len(self.data), 'EROFS xattr 名称或内容越界')
            name = self.data[at + 4:at + 4 + namelen].decode('ascii')
            value = self.data[at + 4 + namelen:end]
            key = (index, name)
            require(key not in attrs, 'EROFS xattr 重复')
            attrs[key] = value
            return (4 + namelen + valuelen + 3) & ~3

        if count:
            pos = offset + inode_size
            total = 12 + (count - 1) * 4
            require(pos + total <= len(self.data), 'EROFS inode xattr 越界')
            shared = self.data[pos + 4]
            require(12 + shared * 4 <= total, 'EROFS shared xattr 索引越界')
            for i in range(shared):
                index = struct.unpack_from('<I', self.data, pos + 12 + i * 4)[0]
                entry(self.xattr * 4096 + index * 4)
            at = pos + 12 + shared * 4
            while at < pos + total:
                at += entry(at)
            require(at == pos + total, 'EROFS inode xattr 长度不同')
        label = attrs.get((6, 'selinux'))
        context = label.rstrip(b'\0').decode('ascii') if label else None
        require(context is None or context.startswith('u:object_r:') and '\t' not in context and '\n' not in context,
                'EROFS 原 SELinux 标签格式无效：' + path)
        return {'mode': mode & 0o7777, 'uid': uid, 'gid': gid, 'selinux': context,
                'selinux_xattr_hex': label.hex() if label else None, 'original_nid': nid,
                'inode_type': mode & 0o170000}
