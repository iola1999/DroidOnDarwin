#!/usr/bin/env python3
"""校验固定 HyperOS-AVD OS3 发布包，流式提取只读扫描所需的分区。"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import tarfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_SHA256 = '7785f61d0e1846fa14d9f3af9db49c905a02604128747a0d617eeff381504ceb'
PREFIX_BYTES = 4 * 1024 * 1024
OUTPUT_LIMIT = 8 * 1024**3
SELECTED = {'images/system.img', 'images/vendor.img', 'images/build.prop',
            'images/source.properties', 'images/kernel_cmdline.txt',
            'images/VerifiedBootParams.textproto', 'images/advancedFeatures.ini'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def part_ranges(prefix, total):
    """只接受固定发布构建器生成的 GPT/liblp 单设备线性布局。"""
    require(len(prefix) == PREFIX_BYTES, '系统磁盘前缀不完整')
    header = bytearray(prefix[512:604])
    require(header[:8] == b'EFI PART' and struct.unpack_from('<I', header, 12)[0] == 92, 'GPT 头格式不匹配')
    expected = struct.unpack_from('<I', header, 16)[0]
    struct.pack_into('<I', header, 16, 0)
    require(zlib.crc32(header) == expected, 'GPT 头 CRC 不匹配')
    start, count, size = struct.unpack_from('<QII', header, 72)
    require(size == 128 and 0 < count <= 128 and start * 512 + count * size <= len(prefix), 'GPT 表范围无效')
    table = prefix[start * 512:start * 512 + count * size]
    require(zlib.crc32(table) == struct.unpack_from('<I', header, 88)[0], 'GPT 表 CRC 不匹配')
    super_ranges = []
    for i in range(count):
        entry = table[i * size:(i + 1) * size]
        if not any(entry[:16]):
            continue
        first, last = struct.unpack_from('<QQ', entry, 32)
        require(first <= last and (last + 1) * 512 <= total, 'GPT 分区越界')
        if entry[56:128].decode('utf-16le').rstrip('\0') == 'super':
            super_ranges.append((first * 512, (last + 1) * 512))
    require(len(super_ranges) == 1, '需要唯一 super 分区')
    base, end = super_ranges[0]
    require(base + 12288 + 65536 <= len(prefix), 'liblp 元数据超出前缀范围')
    geometry = bytearray(prefix[base + 4096:base + 4096 + 52])
    require(struct.unpack_from('<II', geometry) == (0x616c4467, 52), 'liblp geometry 格式不匹配')
    checksum = bytes(geometry[8:40])
    geometry[8:40] = bytes(32)
    require(hashlib.sha256(geometry).digest() == checksum, 'liblp geometry 摘要不匹配')
    require(struct.unpack_from('<III', geometry, 40) == (65536, 2, 4096), 'liblp geometry 布局不匹配')
    data = prefix[base + 12288:base + 12288 + 65536]
    require(struct.unpack_from('<IHHI', data) == (0x414c5030, 10, 0, 128), 'liblp header 格式不匹配')
    lp_header = bytearray(data[:128])
    checksum = bytes(lp_header[12:44])
    lp_header[12:44] = bytes(32)
    require(hashlib.sha256(lp_header).digest() == checksum, 'liblp header 摘要不匹配')
    table_size = struct.unpack_from('<I', lp_header, 44)[0]
    require(table_size <= len(data) - 128, 'liblp 表越界')
    tables = data[128:128 + table_size]
    require(hashlib.sha256(tables).digest() == lp_header[48:80], 'liblp 表摘要不匹配')
    entries = []
    for index, expected_size in enumerate((52, 24, 48, 64)):
        off, count, size = struct.unpack_from('<III', lp_header, 80 + index * 12)
        require(size == expected_size and off + count * size <= table_size, 'liblp 条目范围无效')
        entries.append([tables[off + i * size:off + (i + 1) * size] for i in range(count)])
    require(len(entries[3]) == 1, '需要单设备 liblp')
    extents = [struct.unpack('<QIQI', entry) for entry in entries[1]]
    partitions, occupied = [], []
    for entry in entries[0]:
        raw_name, attributes, first, count, group = struct.unpack('<36sIIII', entry)
        name = raw_name.split(b'\0')[0].decode('ascii')
        require(first + count <= len(extents) and count > 0, '分区 extent 范围无效')
        ranges, size = [], 0
        for sectors, kind, sector, device in extents[first:first + count]:
            source, length = base + sector * 512, sectors * 512
            require(kind == 0 and device == 0 and length > 0, '只接受单设备线性 extent')
            require(base + 1024 * 1024 <= source and source + length <= end, 'extent 越界')
            ranges.append({'source': source, 'length': length, 'target': size})
            occupied.append((source, source + length))
            size += length
        partitions.append({'name': name, 'attributes': attributes, 'group': group, 'size': size, 'ranges': ranges})
    require(len({p['name'] for p in partitions}) == len(partitions), '重复分区名')
    occupied.sort()
    require(all(a[1] <= b[0] for a, b in zip(occupied, occupied[1:])), 'extent 相互重叠')
    require({p['name'] for p in partitions} == {'system', 'vendor', 'system_dlkm'}, '发布分区集合不匹配')
    return partitions


def copy_member(stream, target, info, partitions=False, budget=OUTPUT_LIMIT, partition='system'):
    hasher = hashlib.sha256()
    prefix = stream.read(PREFIX_BYTES) if partitions else b''
    layout = part_ranges(prefix, info['size']) if partitions else None
    selected = next(p for p in layout if p['name'] == partition) if layout else None
    ranges = selected['ranges'] if selected else [{'source': 0, 'length': info['size'], 'target': 0}]
    output_size = selected['size'] if selected else info['size']
    require(output_size <= budget, '分区超过输出预算')
    position = 0
    with target.open('xb') as output:
        block = prefix or stream.read(PREFIX_BYTES)
        while block:
            hasher.update(block)
            require(position + len(block) <= info['size'], '成员超过清单大小')
            for region in ranges:
                first, last = max(position, region['source']), min(position + len(block), region['source'] + region['length'])
                if first < last:
                    output.seek(region['target'] + first - region['source'])
                    output.write(block[first - position:last - position])
            position += len(block)
            block = stream.read(PREFIX_BYTES)
        output.truncate(output_size)
    require(position == info['size'] and hasher.hexdigest() == info['sha256'], '发布成员大小或摘要不匹配')
    return {'source_sha256': hasher.hexdigest(), 'source_bytes': position,
            'path': str(target.relative_to(ROOT)), 'bytes': target.stat().st_size,
            'sha256': digest(target), 'partitions': layout}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-directory', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--partition', choices=('system', 'vendor'), default='system',
                        help='从 system.img 的 liblp 中提取指定分区')
    args = parser.parse_args()
    release, out = args.release_directory.resolve(), args.output.resolve()
    require(release.is_relative_to(ROOT / 'work') and out.is_relative_to(ROOT / 'work'), '输入输出必须在项目 work 内')
    manifest_path = release / 'manifest.json'
    require(digest(manifest_path) == MANIFEST_SHA256, '固定发布清单摘要不匹配')
    manifest = json.loads(manifest_path.read_text())
    parts = [release / part['name'] for part in manifest['parts']]
    for path, info in zip(parts, manifest['parts']):
        require(path.is_file() and path.stat().st_size == info['size'] and digest(path) == info['sha256'], '分卷校验失败：' + path.name)
    require(shutil.disk_usage(out.parent).free >= OUTPUT_LIMIT + 20 * 1024**3, '剩余空间不足以保留构建预算')
    out.mkdir(exist_ok=False)
    report = {'release': 'v0.1.0-a16-hyperos3-fuxi-r1', 'manifest_sha256': MANIFEST_SHA256,
              'source': 'Third-party MysticGSI port; not an official OTA',
              'partition': args.partition, 'script_sha256': digest(Path(__file__).resolve()),
              'parts': manifest['parts'], 'files': {}, 'passed': False}
    process = subprocess.Popen(['/bin/cat', *map(str, parts)], stdout=subprocess.PIPE)
    try:
        seen, budget = set(), 0
        with tarfile.open(fileobj=process.stdout, mode='r|gz') as archive:
            for member in archive:
                name = PurePosixPath(member.name)
                require(not name.is_absolute() and '..' not in name.parts and member.name not in seen, '无效或重复归档路径')
                seen.add(member.name)
                if member.name not in SELECTED:
                    continue
                require(member.isfile() and member.name in manifest['files'], '选定成员不是普通清单文件')
                info = manifest['files'][member.name]
                require(member.size == info['size'], '归档成员大小不匹配')
                selected_name = 'system.ext4' if args.partition == 'system' else 'vendor.erofs'
                target = out / (selected_name if member.name == 'images/system.img' else name.name)
                result = copy_member(archive.extractfile(member), target, info,
                                     member.name == 'images/system.img', OUTPUT_LIMIT - budget, args.partition)
                report['files'][member.name] = result
                budget += result['bytes']
                require(budget <= OUTPUT_LIMIT, '提取结果超过总预算')
                print(json.dumps({'extracted': target.name, 'bytes': result['bytes']}, ensure_ascii=False), flush=True)
        # tar 的结束标记之后可能仍有压缩填充，读完管道后再等待 cat 退出。
        while process.stdout.read(PREFIX_BYTES):
            pass
        require(process.wait(timeout=10) == 0, '读取分卷失败')
        require(set(report['files']) == SELECTED, '发布包缺少选定成员')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        (out / 'extraction.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(out / 'extraction.json')


if __name__ == '__main__':
    main()
