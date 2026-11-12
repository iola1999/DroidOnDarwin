#!/usr/bin/env python3
"""从固定 SDK 系统分区读取部署文件的原始所有者、模式和 SELinux 标签。"""
import ctypes as c
import hashlib
from pathlib import Path
import struct

from prepare_android_runtime import system_partition

LIB = Path('/opt/homebrew/opt/e2fsprogs/lib')


def metadata(image, records, directories, output):
    c.CDLL(str(LIB / 'libcom_err.1.1.dylib'), mode=c.RTLD_GLOBAL)
    lib = c.CDLL(str(LIB / 'libext2fs.2.1.dylib'))
    pointer = c.c_void_p
    ino_type = c.c_uint
    declarations = {
        'ext2fs_open': [c.c_char_p, c.c_int, c.c_int, c.c_int, pointer, c.POINTER(pointer)],
        'ext2fs_close': [pointer],
        'ext2fs_namei_follow': [pointer, ino_type, ino_type, c.c_char_p, c.POINTER(ino_type)],
        'ext2fs_read_inode': [pointer, ino_type, pointer],
        'ext2fs_xattrs_open': [pointer, ino_type, c.POINTER(pointer)],
        'ext2fs_xattrs_read': [pointer],
        'ext2fs_xattr_get': [pointer, c.c_char_p, c.POINTER(pointer), c.POINTER(c.c_size_t)],
        'ext2fs_xattrs_close': [c.POINTER(pointer)],
        'ext2fs_file_open': [pointer, ino_type, c.c_int, c.POINTER(pointer)],
        'ext2fs_file_read': [pointer, pointer, c.c_uint, c.POINTER(c.c_uint)],
        'ext2fs_file_close': [pointer],
    }
    for name, args in declarations.items():
        function = getattr(lib, name)
        function.argtypes = args
        function.restype = c.c_long
    libc = c.CDLL(None)
    libc.free.argtypes = [pointer]
    libc.free.restype = None

    def check(result, operation):
        if result:
            raise ValueError(f'ext4 元数据读取失败：{operation}，错误码 {result}')

    partition = output / 'original-system-metadata.img'
    system_partition(image, partition, 'system')
    fs = pointer()
    check(lib.ext2fs_open(str(partition).encode(), 0, 0, 0,
                         pointer.in_dll(lib, 'unix_io_manager'), c.byref(fs)), 'open')
    items = {}
    try:
        paths = sorted({path for path in [*records, *directories] if path == '/system' or path.startswith('/system/')})
        for path in paths:
            ino = ino_type()
            check(lib.ext2fs_namei_follow(fs, 2, 2, path.encode(), c.byref(ino)), path)
            inode = c.create_string_buffer(128)
            check(lib.ext2fs_read_inode(fs, ino, inode), path)
            mode, uid = struct.unpack_from('<HH', inode.raw)
            gid = struct.unpack_from('<H', inode.raw, 24)[0]
            uid |= struct.unpack_from('<H', inode.raw, 120)[0] << 16
            gid |= struct.unpack_from('<H', inode.raw, 122)[0] << 16
            handle = pointer()
            check(lib.ext2fs_xattrs_open(fs, ino, c.byref(handle)), path)
            value = pointer()
            length = c.c_size_t()
            try:
                check(lib.ext2fs_xattrs_read(handle), path)
                check(lib.ext2fs_xattr_get(handle, b'security.selinux', c.byref(value), c.byref(length)), path)
                label = c.string_at(value, length.value)
            finally:
                libc.free(value)
                check(lib.ext2fs_xattrs_close(c.byref(handle)), path)
            context = label.rstrip(b'\0').decode('ascii')
            if not context.startswith('u:object_r:') or '\t' in context or '\n' in context:
                raise ValueError('原 SELinux 标签格式无效：' + path)
            if path in records:
                file = pointer()
                check(lib.ext2fs_file_open(fs, ino, 0, c.byref(file)), path)
                digest = hashlib.sha256()
                size = 0
                try:
                    buffer = c.create_string_buffer(1 << 20)
                    while True:
                        got = c.c_uint()
                        check(lib.ext2fs_file_read(file, buffer, len(buffer), c.byref(got)), path)
                        if not got.value:
                            break
                        digest.update(buffer.raw[:got.value])
                        size += got.value
                finally:
                    check(lib.ext2fs_file_close(file), path)
                if size != records[path]['bytes'] or digest.hexdigest() != records[path]['sha256']:
                    raise ValueError('元数据镜像文件与已校验原输入不同：' + path)
            items[path] = {'mode': mode & 0o7777, 'uid': uid, 'gid': gid, 'selinux': context,
                           'selinux_xattr_hex': label.hex(), 'original_inode': ino.value}
    finally:
        check(lib.ext2fs_close(fs), 'close')

    return items
