#!/usr/bin/env python3
"""从已验证桌面镜像准备使用原始 SDK36 运行库的独立候选。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from prepare_stock_art import verify as verify_art
from probe_lkl_translation import validate_android_image

ROOT = Path(__file__).resolve().parents[1]
E2FS = Path('/opt/homebrew/opt/e2fsprogs/sbin')


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def identity(path):
    return {'path': str(path.resolve()), 'sha256': sha(path), 'bytes': path.stat().st_size}


def replacements(prepared):
    files = {}
    for guest, record in prepared['files'].items():
        if record['kind'] != 'original':
            continue
        names = [guest]
        if guest.endswith('.so'):
            names.append('/system/lib64/' + Path(guest).name)
        for target in names:
            item = {key: record[key] for key in ('source_relative', 'sha256', 'bytes', 'mode')}
            item['prepared_guest'] = guest
            if target in files and files[target] != item:
                raise ValueError('原始库目标有多个来源：' + target)
            files[target] = item
    return files


def quote(value):
    value = str(value)
    if any(c in value for c in ('"', '\\', '\n', '\r')):
        raise ValueError('debugfs 路径包含不支持的字符')
    return '"' + value + '"'


def execute(command, log):
    with log.open('w') as output:
        subprocess.run(list(map(str, command)), check=True, stdout=output,
                       stderr=subprocess.STDOUT, timeout=300)


def verify(directory):
    report = json.loads((directory / 'manifest.json').read_text())
    if report['scope'] != 'original-sdk36-runtime-desktop-candidate' or report['runtime_executed'] is not False:
        raise ValueError('候选镜像阶段不匹配')
    for item in (report['base_image'], report['base_report'], report['prepared_manifest'], report['generator']):
        if identity(Path(item['path'])) != item:
            raise ValueError('候选输入变化：' + item['path'])
    base = Path(report['base_image']['path']).parent
    validate_android_image(ROOT, base)
    prepared = verify_art(Path(report['prepared_manifest']['path']).parent)
    if replacements(prepared) != report['replacements']:
        raise ValueError('候选文件清单与原始输入不一致')
    image = directory / 'system.img'
    if identity(image) != report['image']:
        raise ValueError('候选系统镜像缺失或摘要变化')
    if report['export_verified'] is not True or report['filesystem_checked'] is not True:
        raise ValueError('候选文件或文件系统尚未检查')
    return report


def prepare(base, prepared_dir, output):
    base, prepared_dir, output = base.resolve(), prepared_dir.resolve(), output.resolve()
    if not output.is_relative_to(ROOT / 'work'):
        raise ValueError('候选输出必须在项目 work 内')
    validate_android_image(ROOT, base)
    prepared = verify_art(prepared_dir)
    files = replacements(prepared)
    original = identity(base / 'system.img')
    base_report = identity(base / 'lkl-summary.json')
    prepared_manifest = identity(prepared_dir / 'manifest.json')
    output.mkdir(parents=True, exist_ok=False)
    image = output / 'system.img'
    execute(['cp', '-c', base / 'system.img', image], output / 'clone.log')
    execute([E2FS / 'e2fsck', '-fn', image], output / 'base-fsck.log')
    # Added APEX paths need space even where the flattened library replaces a
    # current file. The extra capacity is recorded and belongs to this candidate.
    extra = ((sum(record['bytes'] for record in files.values()) + (64 << 20) + (64 << 20) - 1) // (64 << 20)) * (64 << 20)
    with image.open('r+b') as disk:
        disk.truncate(original['bytes'] + extra)
    execute([E2FS / 'resize2fs', image], output / 'resize.log')
    parents = set()
    for guest in files:
        parents.update(str(parent) for parent in Path(guest).parents if str(parent) != '/')
    commands = ['mkdir ' + quote(name) for name in sorted(parents, key=lambda name: (name.count('/'), name))]
    for guest, record in files.items():
        source = prepared_dir / 'root' / record['prepared_guest'].lstrip('/')
        commands += ['rm ' + quote(guest), 'write ' + quote(source) + ' ' + quote(guest),
                     'set_inode_field ' + quote(guest) + ' mode 0' + oct(0o100000 | record['mode'])[2:]]
    script = output / 'replace.debugfs'
    script.write_text('\n'.join(commands) + '\n')
    execute([E2FS / 'debugfs', '-w', '-f', script, image], output / 'replace.log')
    exported = output / 'verify-files'
    exported.mkdir()
    script = output / 'export.debugfs'
    script.write_text('\n'.join('dump -p ' + quote(guest) + ' ' + quote(exported / str(index))
                                for index, guest in enumerate(files)) + '\n')
    execute([E2FS / 'debugfs', '-f', script, image], output / 'export.log')
    for index, (guest, record) in enumerate(files.items()):
        path = exported / str(index)
        if (not path.is_file() or path.stat().st_size != record['bytes'] or
                sha(path) != record['sha256'] or path.stat().st_mode & 0o777 != record['mode']):
            raise ValueError('ext4 导出文件与原始输入不同：' + guest)
    execute([E2FS / 'e2fsck', '-fn', image], output / 'candidate-fsck.log')
    if identity(base / 'system.img') != original or identity(base / 'lkl-summary.json') != base_report:
        raise ValueError('准备期间基准镜像或报告变化')
    if identity(prepared_dir / 'manifest.json') != prepared_manifest:
        raise ValueError('准备期间原始组件清单变化')
    verify_art(prepared_dir)
    shutil.rmtree(exported)
    report = {'scope': 'original-sdk36-runtime-desktop-candidate', 'runtime_executed': False,
              'base_image': original, 'base_report': base_report, 'prepared_manifest': prepared_manifest,
              'generator': identity(Path(__file__)), 'image': identity(image), 'replacements': files,
              'extra_capacity_bytes': extra, 'export_verified': True, 'filesystem_checked': True,
              'limitations': ['Non-isolated project linker namespaces and service bootstrap remain.',
                              'Project graphics and service helper files remain; original init has not run.',
                              'No desktop, APK, performance or iPhone validation has run on this image.']}
    (output / 'manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    return verify(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path)
    parser.add_argument('--prepared-art', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--verify', type=Path)
    args = parser.parse_args()
    if args.verify:
        if any((args.base, args.prepared_art, args.output)):
            parser.error('--verify 只能单独使用')
        report = verify(args.verify.resolve())
    else:
        if not all((args.base, args.prepared_art, args.output)):
            parser.error('需要 --base、--prepared-art 与 --output')
        report = prepare(args.base, args.prepared_art, args.output)
    print(json.dumps({'prepared': True, 'files': len(report['replacements']), 'image': report['image'],
                      'runtime_executed': False}, ensure_ascii=False))


if __name__ == '__main__':
    main()
