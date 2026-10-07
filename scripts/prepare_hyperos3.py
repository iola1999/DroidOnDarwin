#!/usr/bin/env python3
"""将 HyperOS 3 ext4 GSI 加入 Husk v12 系统盘；输出到一个新目录。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import uuid
import zlib

BASE_SHA256 = 'a8dbaecc8fdcd682991b078d6bbb7df6972e459208634fa8dcbad24f17208d6a'
SECTOR = 512
MIB = 1024 * 1024
DEBUGFS = shutil.which('debugfs') or '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True).stdout


def digest(path):
    with path.open('rb') as source:
        h = hashlib.sha256()
        for block in iter(lambda: source.read(8 * MIB), b''):
            h.update(block)
    return h.hexdigest()


def align(value, unit):
    return (value + unit - 1) // unit * unit


def extract(source, offset, length, dest):
    with source.open('rb') as src, dest.open('wb') as dst:
        src.seek(offset)
        while length:
            block = src.read(min(length, 8 * MIB))
            if not block:
                raise ValueError('镜像内容不完整')
            dst.write(block)
            length -= len(block)


def put(source, dest, offset):
    with source.open('rb') as src, dest.open('r+b') as dst:
        dst.seek(offset)
        shutil.copyfileobj(src, dst, 8 * MIB)


def ext4_write(image, source, target, context):
    # debugfs 命令中的路径单独加引号；SELinux 标签在重建 inode 后恢复。
    def quote(s):
        return '"' + str(s).replace('\\', '\\\\').replace('"', '\\"') + '"'
    commands = image.parent / 'edit.debugfs'
    commands.write_text(f'rm {quote(target)}\nwrite {quote(source)} {quote(target)}\n'
                        f'set_inode_field {quote(target)} mode 0100644\n'
                        f'ea_set {quote(target)} security.selinux {context}\\000\n')
    run(DEBUGFS, '-w', '-f', commands, image)
    check = image.parent / 'readback.bin'
    run(DEBUGFS, '-R', f'dump {quote(target)} {quote(check)}', image)
    if check.read_bytes() != source.read_bytes():
        raise ValueError(f'写入校验失败：{target}')
    check.unlink()


def patch_cpio(data):
    """保留 newc 各条目的权限和所有者，只替换 GSI fstab 的设备路径。"""
    result = bytearray()
    cursor = 0
    replaced = 0
    while True:
        header = bytearray(data[cursor:cursor + 110])
        if header[:6] != b'070701':
            raise ValueError('vendor ramdisk 不是预期的 newc 格式')
        size = int(header[54:62], 16)
        name_size = int(header[94:102], 16)
        name = data[cursor + 110:cursor + 110 + name_size]
        begin = align(cursor + 110 + name_size, 4)
        contents = data[begin:begin + size]
        if name.rstrip(b'\0') == b'system/etc/fstab.virtio.gsi.vdc':
            if contents.count(b'/dev/block/vdc ') != 2:
                raise ValueError('GSI fstab 与 Husk v12 不一致')
            contents = contents.replace(b'/dev/block/vdc ', b'/dev/block/by-name/hyperos ')
            header[54:62] = f'{len(contents):08x}'.encode()
            replaced += 1
        result.extend(header + name)
        result.extend(bytes(-len(result) % 4))
        result.extend(contents)
        result.extend(bytes(-len(result) % 4))
        cursor = align(begin + size, 4)
        if name.rstrip(b'\0') == b'TRAILER!!!':
            break
    if replaced != 1:
        raise ValueError('vendor ramdisk 缺少唯一的 GSI fstab')
    result.extend(bytes(-len(result) % 512))
    return result


def patch_vendor_boot(image, scratch):
    data = image.read_bytes()
    if data[:8] != b'VNDRBOOT' or struct.unpack_from('<I', data, 8)[0] != 4:
        raise ValueError('仅支持 Husk v12 的 vendor_boot v4')
    page = struct.unpack_from('<I', data, 12)[0]
    ramdisk_size = struct.unpack_from('<I', data, 24)[0]
    header_size, dtb_size = struct.unpack_from('<II', data, 2096)
    table_size, count, entry_size, config_size = struct.unpack_from('<IIII', data, 2112)
    if count != 1 or entry_size != 108 or dtb_size != 0:
        raise ValueError('vendor_boot 的片段布局与 Husk v12 不一致')
    begin = align(header_size, page)
    table_start = begin + align(ramdisk_size, page)
    config_start = table_start + align(table_size, page)
    packed, unpacked = scratch / 'ramdisk.lz4', scratch / 'ramdisk.cpio'
    packed.write_bytes(data[begin:begin + ramdisk_size])
    run('lz4', '-d', '-f', packed, unpacked)
    unpacked.write_bytes(patch_cpio(unpacked.read_bytes()))
    run('lz4', '-l', '-9', '-f', unpacked, packed)
    compressed = packed.read_bytes()
    header = bytearray(data[:begin])
    struct.pack_into('<I', header, 24, len(compressed))
    table = bytearray(data[table_start:table_start + table_size])
    struct.pack_into('<I', table, 0, len(compressed))
    result = header + compressed.ljust(align(len(compressed), page), b'\0')
    result += table.ljust(align(table_size, page), b'\0')
    result += data[config_start:config_start + config_size]
    if len(result) > len(data):
        raise ValueError('vendor_boot 分区容量不足')
    image.write_bytes(result.ljust(len(data), b'\0'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True, help='原始 vda-v12.qcow2')
    parser.add_argument('--system', type=Path, required=True, help='已提取的 HyperOS 3 ext4 system.img')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if digest(args.base) != BASE_SHA256:
        raise ValueError('基础镜像 SHA-256 与 Husk v12 发布文件不同')
    with args.system.open('rb') as src:
        src.seek(1080)
        if src.read(2) != b'\x53\xef':
            raise ValueError('--system 需要原始 ext4 分区镜像')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    scratch = out / 'work'
    scratch.mkdir()
    disk, gsi = scratch / 'disk.raw', scratch / 'system.img'
    print('转换基础磁盘并复制 system', flush=True)
    run('qemu-img', 'convert', '-f', 'qcow2', '-O', 'raw', args.base, disk)
    run('cp', '-c', args.system.resolve(), gsi)
    policy = scratch / 'plat_sepolicy.cil'
    run(DEBUGFS, '-R', f'dump /system/etc/selinux/plat_sepolicy.cil {policy}', gsi)
    lines = policy.read_text().splitlines(keepends=True)
    for class_name, permission in [('system', 'firmware_load'), ('netlink_route_socket', 'nlmsg')]:
        matches = [i for i, line in enumerate(lines) if line.startswith(f'(class {class_name} (')]
        if len(matches) != 1:
            raise ValueError(f'找不到唯一的 SELinux class：{class_name}')
        i = matches[0]
        if permission not in lines[i].replace(')', ' ').split():
            lines[i] = lines[i].replace('))', f' {permission}))')
    policy.write_text(''.join(lines))
    ext4_write(gsi, policy, '/system/etc/selinux/plat_sepolicy.cil', 'u:object_r:sepolicy_file:s0')
    ext4_write(gsi, Path(__file__).parent / 'hyperos3/init.rc', '/system/etc/init/husk-hyperos3.rc', 'u:object_r:system_file:s0')
    ethernet = scratch / 'ethernet.xml'
    ethernet.write_text('<permissions><feature name="android.hardware.ethernet" /></permissions>\n')
    ext4_write(gsi, ethernet, '/system/etc/permissions/husk-ethernet.xml', 'u:object_r:system_file:s0')

    with disk.open('rb') as src:
        mbr = bytearray(src.read(SECTOR))
        header = bytearray(src.read(SECTOR))
        if header[:8] != b'EFI PART':
            raise ValueError('基础磁盘缺少 GPT')
        table_lba, count, entry_size = struct.unpack_from('<QII', header, 72)
        src.seek(table_lba * SECTOR)
        table = bytearray(src.read(count * entry_size))
    partitions = {}
    vacant = []
    for i in range(count):
        entry = table[i * entry_size:(i + 1) * entry_size]
        if not any(entry[:16]):
            vacant.append(i)
            continue
        first, last = struct.unpack_from('<QQ', entry, 32)
        partitions[entry[56:128].decode('utf-16le').rstrip('\0')] = (first * SECTOR, (last - first + 1) * SECTOR)
    print('更新 ramdisk、GRUB 和独立 metadata', flush=True)
    for name in ['vendor_boot', 'EFI', 'persist']:
        extract(disk, *partitions[name], scratch / f'{name}.img')
    patch_vendor_boot(scratch / 'vendor_boot.img', scratch)
    grub = scratch / 'grub.cfg'
    run('mcopy', '-i', scratch / 'EFI.img', '::/boot/grub/grub.cfg', grub)
    content = grub.read_text()
    entry = 'linux ${boot_partition}/kernel '
    if content.count(entry) != 1:
        raise ValueError('GRUB 内核入口与 Husk v12 不一致')
    grub.write_text(content.replace(entry, entry + 'drm_kms_helper.fbdev_emulation=0 '))
    run('mcopy', '-o', '-i', scratch / 'EFI.img', grub, '::/boot/grub/grub.cfg')
    env = scratch / 'grubenv'
    run('mcopy', '-i', scratch / 'persist.img', '::/grubenv', env)
    original = env.read_bytes()
    settings = dict(line.split('=', 1) for line in original.decode().splitlines() if '=' in line and not line.startswith('#'))
    settings.update(android_selinux_permissive='1', android_insecure_adb='1', android_nobootanim='1',
                    android_low_perf='1', grub_timeout='1', kms_display_resolution='360x800', android_lcd_density='160',
                    grub_android_default='advancedoptions-virtio_common>android-virtio_common-gsifromvdc')
    content = '# GRUB Environment Block\n' + ''.join(f'{k}={v}\n' for k, v in settings.items())
    if len(content.encode()) > len(original):
        raise ValueError('GRUB 环境块容量不足')
    env.write_bytes(content.encode().ljust(len(original), b'#'))
    run('mcopy', '-o', '-i', scratch / 'persist.img', env, '::/grubenv')
    for name in ['vendor_boot', 'EFI', 'persist']:
        put(scratch / f'{name}.img', disk, partitions[name][0])
    with disk.open('r+b') as dest:
        dest.seek(partitions['metadata'][0])
        dest.write(bytes(partitions['metadata'][1]))

    # 追加 GPT 分区，保留已有分区和 super 内的 vendor 布局。
    start = align(disk.stat().st_size, MIB)
    length = align(gsi.stat().st_size, SECTOR)
    total = align(start + length + (count * entry_size) + 2 * SECTOR, MIB)
    last_lba = total // SECTOR - 1
    new_entry = bytearray(entry_size)
    new_entry[:16] = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le
    new_entry[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQ', new_entry, 32, start // SECTOR, (start + length) // SECTOR - 1)
    new_entry[56:70] = 'hyperos'.encode('utf-16le')
    table[vacant[0] * entry_size:(vacant[0] + 1) * entry_size] = new_entry
    table_sectors = align(len(table), SECTOR) // SECTOR
    def gpt(current, backup, table_start):
        result = bytearray(header)
        struct.pack_into('<QQ', result, 24, current, backup)
        struct.pack_into('<Q', result, 48, last_lba - table_sectors - 1)
        struct.pack_into('<Q', result, 72, table_start)
        struct.pack_into('<I', result, 88, zlib.crc32(table))
        struct.pack_into('<I', result, 16, 0)
        struct.pack_into('<I', result, 16, zlib.crc32(result[:struct.unpack_from('<I', result, 12)[0]]))
        return result
    struct.pack_into('<I', mbr, 458, min(last_lba, 0xffffffff))
    with disk.open('r+b') as dest:
        dest.truncate(total)
        dest.write(mbr)
        dest.write(gpt(1, last_lba, table_lba))
        dest.seek(table_lba * SECTOR)
        dest.write(table)
        dest.seek((last_lba - table_sectors) * SECTOR)
        dest.write(table)
        dest.write(gpt(last_lba, 1, last_lba - table_sectors))
    put(gsi, disk, start)
    print('生成独立 qcow2 和空数据盘', flush=True)
    run('qemu-img', 'convert', '-f', 'raw', '-O', 'qcow2', disk, out / 'lineage-vda.qcow2')
    run('qemu-img', 'create', '-f', 'qcow2', out / 'lineage-vdb.qcow2', '16G')
    (out / 'lineage-guest.version').write_text('v12')
    (out / 'lineage-vdb.qcow2.seed').write_text('v10')
    (out / 'husk-custom-guest').write_text('hyperos-3\n')
    manifest = {'base_sha256': BASE_SHA256, 'source_system_sha256': digest(args.system),
                'prepared_system_sha256': digest(out / 'lineage-vda.qcow2'),
                'virtual_size': total, 'resolution': '360x800', 'selinux': 'Permissive'}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(out, flush=True)


if __name__ == '__main__':
    main()
