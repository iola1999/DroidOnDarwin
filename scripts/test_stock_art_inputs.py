#!/usr/bin/env python3
"""检查真实 ART 组件目录的缺文件、改写、权限和 manifest 不一致。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from prepare_stock_art import verify
from stock_art import attach


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    source = args.output.resolve()
    original = verify(source)
    libraries = {Path(name).name for name in original['inputs'] if name.endswith('.so')}
    public = source / 'root/system/etc/public.libraries.txt'
    assert original['files']['/system/etc/public.libraries.txt']['kind'] == 'original'
    for line in public.read_text().splitlines():
        fields = line.split('#', 1)[0].split()
        if fields and not {'nopreload', '32'} & set(fields[1:]):
            assert fields[0] in libraries, fields[0]
    assert all(set(record.get('needed', ())) <= libraries for record in original['inputs'].values())
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
        for name, relative in [('missing_public_library_list', 'etc/public.libraries.txt'),
                               ('missing_preloaded_library', 'lib64/libcamera2ndk.so')]:
            required = target / 'root/system' / relative
            saved, permissions = required.read_bytes(), required.stat().st_mode & 0o777
            required.unlink()
            rejected(name)
            required.write_bytes(saved)
            required.chmod(permissions)
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
        if original.get('runtime_source') == 'hyperos':
            modified = json.loads(json.dumps(original))
            del modified['files'][selected]
            manifest.write_text(json.dumps(modified))
            file.unlink()
            rejected('missing_original_and_manifest_entry')
            file.write_bytes(initial)
            file.chmod(mode)
        page_bytes = original.get('guest_page_bytes', 16384)
        wrong_page_bytes = 4096 if page_bytes == 16384 else 16384
        wrong_source = 'sdk36' if original.get('runtime_source') == 'hyperos' else 'hyperos'
        for key, value in [('runtime_source', wrong_source), ('guest_page_bytes', wrong_page_bytes), ('dependencies_sha256', '0' * 64)]:
            modified = json.loads(json.dumps(original))
            modified[key] = value
            manifest.write_text(json.dumps(modified))
            rejected('wrong_' + key)
        manifest.write_text(json.dumps(original))
        assert verify(target)
        assert attach(target, target, guest_page_bytes=page_bytes)
        try:
            attach(target, target, guest_page_bytes=wrong_page_bytes)
        except ValueError as error:
            tests.append({'case': 'runtime_page_mismatch', 'rejected': True, 'error': str(error)})
        else:
            raise AssertionError('runtime_page_mismatch')
    print(json.dumps({'actual_manifest_accepted': True, 'negative_cases': tests}, ensure_ascii=False))


if __name__ == '__main__':
    main()
