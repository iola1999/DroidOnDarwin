"""固定原始 runtime 输入及组件运行结果的检查。"""
import hashlib
import json
from pathlib import Path
import shutil
import struct

FILES = ('bin/linker64', 'lib64/bionic/libc.so', 'lib64/bionic/libdl.so', 'lib64/bionic/libm.so')
MANIFESTS = {
    'sdk36': '4b1a45b0ee51725c68f1c29765958d0b60b45a5f2325bedfc61764cd49232249',
    'hyperos': 'c9ec384e1eee4c754152fddaae8d351ff5f1adaafb86e5cdc88fb02f5f712d0a',
}
CHECKS = ('page_bss', 'allocation', 'allocation_contents', 'thread_tls', 'dynamic_libm')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(source, directory, manifest_path, out):
    directory, manifest_path = directory.resolve(), manifest_path.resolve()
    if sha(manifest_path) != MANIFESTS[source]:
        raise ValueError('原始 runtime 清单摘要不匹配')
    manifest = json.loads(manifest_path.read_text())
    records = {}
    for name in FILES:
        key = ('com.android.runtime/' if source == 'sdk36' else
               '/system/apex/com.android.runtime.apex!apex_payload.img/') + name
        expected = manifest['files'][key]
        if source == 'hyperos':
            expected = expected['sha256']
        path = directory / name
        data = path.read_bytes()
        if sha(path) != expected:
            raise ValueError('原始 runtime 文件摘要不匹配：' + name)
        if data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', data, 16) != (3, 183):
            raise ValueError('需要 AArch64 ET_DYN 文件：' + name)
        records[name] = {'path': str(path), 'sha256': expected, 'bytes': len(data)}
    output = out / 'stock-runtime'
    output.mkdir(parents=True, exist_ok=True)
    for name, record in records.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(record['path'], target)
        if sha(target) != record['sha256']:
            raise ValueError('runtime 复制后摘要不匹配：' + name)
    shutil.copyfile(manifest_path, output / 'source-manifest.json')
    (out / 'stock-ld.config.txt').write_text('dir.stock = /system/bin\n\n[stock]\n'
        'namespace.default.isolated = false\n'
        'namespace.default.search.paths = /apex/com.android.runtime/lib64/bionic\n')
    return {'source': source, 'directory': str(directory), 'manifest': str(manifest_path),
            'manifest_sha256': sha(manifest_path), 'identity':
            {key: manifest[key] for key in ('url', 'archive_sha1')} if source == 'sdk36' else manifest['source'],
            'files': records}


def unchanged(identity):
    return sha(Path(identity['manifest'])) == identity['manifest_sha256'] and all(
        sha(Path(record['path'])) == record['sha256'] for record in identity['files'].values())


def validate(rows, guest_page_bytes=16384):
    stages = [r for r in rows if 'component_stage' in r]
    starts = [r for r in rows if 'component_start' in r]
    main = [r for r in rows if 'component_main' in r]
    checks = [r for r in rows if 'component_check' in r]
    objects = [r for r in rows if 'component_object' in r]
    supervisor = {'component_supervisor': True, 'page_size': guest_page_bytes}
    complete = {'test': 'stock_bionic_observation_complete', 'passed': True}
    if [r.get('component_stage') for r in stages] != ['linker_help', 'dynamic']:
        return False
    if [r for r in rows if 'component_supervisor' in r] != [supervisor]:
        return False
    if [r for r in rows if r.get('test') == 'stock_bionic_observation_complete'] != [complete]:
        return False
    if any(set(r) != {'component_object', 'base'} or type(r['component_object']) is not str or
           type(r['base']) is not int or not 0 <= r['base'] < 2**64 for r in objects):
        return False
    events = [r for r in rows if any(key in r for key in
              ('component_supervisor', 'component_start', 'component_stage', 'component_main',
               'component_object', 'component_check')) or
              r.get('test') == 'stock_bionic_observation_complete']
    for stage in stages:
        if stage.get('started') is False:
            continue
        if set(stage) != {'component_stage', 'started', 'success', 'exec_errno', 'exit_code', 'signal', 'usage'}:
            return False
        if stage['started'] is not True or type(stage['success']) is not bool or type(stage['usage']) is not bool:
            return False
        if any(type(stage[k]) is not int for k in ('exec_errno', 'exit_code', 'signal')):
            return False
        error, code, signal = (stage[k] for k in ('exec_errno', 'exit_code', 'signal'))
        if not 0 <= error <= 4095 or not -1 <= code <= 255 or not 0 <= signal <= 64:
            return False
        if (code == -1) != bool(signal) or (error and (code != 127 or signal)):
            return False
        success = error == 0 and code == 0 and signal == 0
        if stage['component_stage'] == 'linker_help':
            success = success and stage['usage']
        if stage['success'] != success:
            return False
    help, dynamic = stages
    if not help.get('started'):
        return False
    if not help['success']:
        return (starts == [{'component_start': 'linker_help'}] and not main and not checks and not objects and
                dynamic == {'component_stage': 'dynamic', 'started': False, 'reason': 'linker_failed'} and
                events == [supervisor, starts[0], help, dynamic, complete])
    if starts != [{'component_start': 'linker_help'}, {'component_start': 'dynamic'}] or dynamic.get('started') is not True:
        return False
    if len(main) > 1 or ((checks or objects) and not main):
        return False
    if main and main != [{'component_main': True, 'aux_page_size': guest_page_bytes, 'page_size': guest_page_bytes}]:
        return False
    if [r.get('component_check') for r in checks] != list(CHECKS[:len(checks)]):
        return False
    if any(set(r) != {'component_check', 'success'} or type(r['success']) is not bool for r in checks):
        return False
    if any(not r['success'] for r in checks[:-1]):
        return False
    if events != [supervisor, starts[0], help, starts[1], *main, *objects, *checks, dynamic, complete]:
        return False
    if dynamic['success']:
        return len(main) == 1 and len(checks) == len(CHECKS) and all(r['success'] for r in checks)
    return True
