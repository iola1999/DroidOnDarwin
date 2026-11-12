"""标准 ART 组件的装载输入、成功结果和 maps 导出检查。"""
import json
from pathlib import Path

from prepare_stock_art import verify, sha

MAPS = ('before-vm', 'after-vm', 'after-gc', 'before-warmup', 'after-warmup')
TOKEN = 0x1122334455667788
VALUES = {'vm_absent': 0, 'create_vm': 0, 'load_dex': 1, 'primitive_arguments': TOKEN,
          'registered_natives': TOKEN, 'gc_observed': None, 'global_reference': TOKEN,
          'array_storage': 47, 'java_threads': 2000, 'native_thread': 58, 'java_exception': 1,
          'warmup_result': sum(((i * 17) ^ (i >> 3)) + 9 for i in range(200000)), 'destroy_vm': 0}


def attach(directory, output, *, guest_page_bytes=16384):
    directory = directory.resolve()
    report = verify(directory)
    if report.get('guest_page_bytes', 16384) != guest_page_bytes:
        raise ValueError('ART 准备目录与客体页大小不匹配')
    lines = []
    for path in report['directories']:
        if any(c.isspace() for c in path) or '..' in Path(path).parts:
            raise ValueError('组件目录包含不支持的路径字符')
        lines.append('D\t' + path + '\n')
    for path, item in report['files'].items():
        if any(c.isspace() for c in path) or '..' in Path(path).parts or item['mode'] not in (0o644, 0o755):
            raise ValueError('组件文件路径或模式不支持')
        lines.append(f'F\t{item["mode"]:04o}\t{path}\n')
    (output / 'stock-art-files.tsv').write_text(''.join(lines))
    (output / 'stock-art-boot-classpath').write_text(report['guest_command'][1] + '\n')
    return {'directory': str(directory), 'manifest_sha256': sha(directory / 'manifest.json'),
            'prepared': report}


def unchanged(identity):
    directory = Path(identity['directory'])
    try:
        return sha(directory / 'manifest.json') == identity['manifest_sha256'] and verify(directory) == identity['prepared']
    except (OSError, ValueError, KeyError):
        return False


def validate(rows, guest_page_bytes=16384, jit_mode="default"):
    if guest_page_bytes not in (4096, 16384) or jit_mode not in ('default', 'first-use', 'background'):
        return False
    events = [row for row in rows if any(key.startswith('stock_art_') for key in row) or
              row.get('test') == 'stock_art_standard_jni']
    checks = [row for row in events if 'stock_art_check' in row]
    if [row['stock_art_check'] for row in checks] != list(VALUES):
        return False
    for row in checks:
        value = row.get('observed')
        if set(row) != {'stock_art_check', 'passed', 'observed'} or row['passed'] is not True or type(value) is not int:
            return False
        expected = VALUES[row['stock_art_check']]
        if (expected is not None and value != expected) or (expected is None and not 1 <= value <= 32):
            return False
    maps = [row for row in events if 'stock_art_maps' in row]
    exports = [row for row in events if 'stock_art_export' in row]
    if [row['stock_art_maps'] for row in maps] != list(MAPS) or [row['stock_art_export'] for row in exports] != list(MAPS):
        return False
    for row, exported in zip(maps, exports):
        if (set(row) != {'stock_art_maps', 'bytes'} or type(row['bytes']) is not int or row['bytes'] <= 0 or
                exported != {'stock_art_export': row['stock_art_maps'], 'present': True, 'bytes': row['bytes']} or
                type(exported['bytes']) is not int or exported['present'] is not True):
            return False
    arrays = [row for row in events if 'stock_art_array_storage' in row]
    if len(arrays) != 1:
        return False
    array = arrays[0]
    if (set(array) != {'stock_art_array_storage', 'address', 'is_copy'} or array['stock_art_array_storage'] is not True or
            type(array['address']) is not int or not 0 < array['address'] < 1 << 64 or
            array['address'] % 4 or type(array['is_copy']) is not bool):
        return False
    expected = [
        {'stock_art_supervisor': True, 'page_size': guest_page_bytes, 'timeout_seconds': 90},
        {'stock_art_config': True, 'jni_version': 0x10006, 'jvalue_bytes': 8, 'jit_requested': True, 'jit_mode': jit_mode},
        maps[0], checks[0], {'stock_art_stage': 'create_vm'}, checks[1], maps[1], *checks[2:7], maps[2],
        array, *checks[7:11], maps[3],
        *[{'stock_art_warmup': round, 'calls': 200000, 'wait_before_ms': 200 if round else 0,
           'observed': VALUES['warmup_result']} for round in range(10 if jit_mode == 'background' else 1)],
        checks[11], {'stock_art_jit': 'unverified', 'warmup_calls': 200000 * (10 if jit_mode == 'background' else 1)}, maps[4],
        {'stock_art_stage': 'destroy_vm'}, checks[12],
        {'test': 'stock_art_standard_jni', 'passed': True, 'jit_execution_confirmed': False},
        {'stock_art_exit': True, 'success': True, 'exec_errno': 0, 'exit_code': 0, 'signal': 0, 'timed_out': False},
        *exports,
    ]
    return events == expected and json.dumps(events, sort_keys=True) == json.dumps(expected, sort_keys=True)


def artifacts(directory, rows):
    result = []
    for name in MAPS:
        path = directory / ('maps-' + name + '.txt')
        exported = [row for row in rows if row.get('stock_art_export') == name]
        record = {'name': name, 'present': path.is_file()}
        if path.is_file():
            record.update(path=str(path), bytes=path.stat().st_size, sha256=sha(path))
        record['validated'] = exported == ([{'stock_art_export': name, 'present': True, 'bytes': record['bytes']}]
                                           if record['present'] else [{'stock_art_export': name, 'present': False}])
        result.append(record)
    return result
