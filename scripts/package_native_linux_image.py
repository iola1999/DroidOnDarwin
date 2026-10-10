#!/usr/bin/env python3
"""生成供两端设备列表导入的 .droidimage 目录包。"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
INPUT = ROOT / 'work/native-linux-nce/processes'


def digest(path):
    with Path(path).open('rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def runtime():
    # Prepared executable layouts are tied to the kernel and generated guest payload.
    return hashlib.sha256(''.join(digest(INPUT / name) for name in
                                 ('lkl-image.bin', 'payload.bin', 'program-images.h')).encode()).hexdigest()


def write_runtime(directory):
    (Path(directory) / 'Runtime.json').write_text(json.dumps({'runtime': runtime()}) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--name', default='Android 16')
    args = parser.parse_args()
    if args.output.suffix != '.droidimage' or args.output.exists():
        parser.error('输出必须是尚不存在的 .droidimage 目录')
    args.output.mkdir(parents=True)
    files = {}
    for name in ('system.img', 'android-data-seed.img'):
        subprocess.run(['cp', '-c', INPUT / name, args.output / name], check=True)
        files[name] = digest(args.output / name)
    (args.output / 'manifest.json').write_text(json.dumps({
        'format': 1, 'name': args.name, 'runtime': runtime(), 'files': files}, ensure_ascii=False, indent=2) + '\n')
    print(args.output)


if __name__ == '__main__':
    main()
