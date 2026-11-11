#!/usr/bin/env python3
"""检查真实 ART 组件目录的缺文件、改写、权限和 manifest 不一致。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from prepare_stock_art import verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.output.resolve()
    original = verify(source)
    tests = []
    with tempfile.TemporaryDirectory(prefix='stock-art-input-test-', dir=source.parent) as temporary:
        target = Path(temporary)
        shutil.copytree(source / 'root', target / 'root')
        manifest = target / 'manifest.json'
        manifest.write_text(json.dumps(original))
        assert verify(target)
        selected = '/apex/com.android.art/lib64/libartpalette.so'
        file = target / 'root' / selected.lstrip('/')
        initial = file.read_bytes()
        mode = file.stat().st_mode & 0o777

        def rejected(name):
            try:
                verify(target)
            except ValueError as error:
                tests.append({'case': name, 'rejected': True, 'error': str(error)})
            else:
                raise AssertionError(name)

        file.unlink()
        rejected('missing_original')
        file.write_bytes(initial)
        file.chmod(mode)
        file.write_bytes(bytes([initial[0] ^ 1]) + initial[1:])
        rejected('changed_original')
        modified = json.loads(json.dumps(original))
        modified['files'][selected]['sha256'] = hashlib.sha256(file.read_bytes()).hexdigest()
        manifest.write_text(json.dumps(modified))
        rejected('changed_original_and_output_manifest')
        file.write_bytes(initial)
        file.chmod(mode ^ 0o100)
        manifest.write_text(json.dumps(original))
        rejected('changed_mode')
        file.chmod(mode)
        extra = target / 'root/data/stock-art/extra'
        extra.write_text('unexpected')
        rejected('extra_file')
        extra.unlink()
        modified = json.loads(json.dumps(original))
        modified['input_manifest_sha256'] = '0' * 64
        manifest.write_text(json.dumps(modified))
        rejected('wrong_runtime_manifest_identity')
        modified = json.loads(json.dumps(original))
        modified['runtime_executed'] = True
        manifest.write_text(json.dumps(modified))
        rejected('wrong_stage')
        manifest.write_text(json.dumps(original))
        assert verify(target)
    print(json.dumps({'actual_manifest_accepted': True, 'negative_cases': tests}, ensure_ascii=False))


if __name__ == '__main__':
    main()
