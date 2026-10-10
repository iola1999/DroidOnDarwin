"""记录离线 ART 输入，校验待装载的 OAT/VDEX 与当前构建一致。"""
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = {'name': 'NceWorkload', 'dex': '/data/nce/NceWorkload.dex',
           'context': 'PCL[]'}
WINDOW_CONTEXT = ('PCL[]{PCL[/system_ext/framework/androidx.window.extensions.jar]#'
                  'PCL[/system_ext/framework/androidx.window.sidecar.jar]}')
DESKTOP = [
    {'name': 'SystemUIGoogle',
     'dex': '/system_ext/priv-app/SystemUIGoogle/SystemUIGoogle.apk',
     'context': WINDOW_CONTEXT},
    {'name': 'Launcher3QuickStep',
     'dex': '/system_ext/priv-app/Launcher3QuickStep/Launcher3QuickStep.apk',
     'context': WINDOW_CONTEXT},
]


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def plan(assets, desktop=False):
    targets = DESKTOP if desktop else [FIXTURE]
    resources = dict(assets)
    missing = {item['dex'] for item in targets} - resources.keys()
    if missing:
        raise RuntimeError('Missing AOT input: ' + ', '.join(sorted(missing)))
    # Classpath fragments define ordering; all packaged JARs cover shared and boot dependencies.
    inputs = {name: digest(path) for name, path in assets
              if name.endswith(('.jar', '.dex')) or '/etc/classpaths/' in name or
              any(name == target['dex'] for target in targets)}
    recipe = {str(path.relative_to(ROOT)): digest(path) for path in (
        Path(__file__).resolve(), ROOT / 'experiments/native-linux/userspace/art-compiler-test.c',
        ROOT / 'experiments/native-linux/userspace/framework-test.cpp')}
    return {'targets': targets, 'input_files': inputs, 'recipe': recipe,
            'compiler_settings': {'filter': 'speed', 'inline_max_code_units': 0,
                                  'expanded_jit': True, 'fast_baseline': False, 'nterp': True,
                                  'max_heap_mib': 256}}


def file_names(current_plan):
    if current_plan.get('kind') == 'boot':
        return {'boot.art', 'boot.oat', 'boot.vdex'}
    return {target['name'] + '.' + extension
            for target in current_plan['targets'] for extension in ('odex', 'vdex')}


def write_manifest(directory, art_inputs, current_plan, compiler_heap_bases):
    if not compiler_heap_bases or any(base <= 0 for base in compiler_heap_bases):
        raise RuntimeError('Compiler heap address was not recorded')
    result = {'format': 1, 'art_inputs': art_inputs, 'plan': current_plan,
              'compiler_heap_bases': sorted(set(compiler_heap_bases)),
              'files': {name: digest(directory / name) for name in sorted(file_names(current_plan))}}
    name = 'boot-manifest.json' if current_plan.get('kind') == 'boot' else 'manifest.json'
    (directory / name).write_text(json.dumps(result, indent=2) + '\n')
    return result


def validate(directory, art_inputs, current_plan):
    name = 'boot-manifest.json' if current_plan.get('kind') == 'boot' else 'manifest.json'
    manifest = json.loads((directory / name).read_text())
    if manifest.get('format') != 1:
        raise RuntimeError('Unsupported AOT manifest format; regenerate the artifact')
    compiler_inputs = {key: value for key, value in manifest['art_inputs'].items()
                       if key != 'offline_compiler'}
    runtime_inputs = {key: value for key, value in art_inputs.items() if key != 'offline_compiler'}
    if compiler_inputs != runtime_inputs or manifest['plan'] != current_plan:
        raise RuntimeError('AOT compiler, recipe or DEX dependencies differ; regenerate the artifact')
    if set(manifest['files']) != file_names(current_plan):
        raise RuntimeError('AOT artifact file set is incomplete or unexpected')
    for name, expected in manifest['files'].items():
        if digest(directory / name) != expected:
            raise RuntimeError('AOT artifact checksum mismatch: ' + name)
    return manifest


def boot_plan(assets):
    return {'kind': 'boot',
            'input_files': {name: digest(path) for name, path in assets if name.endswith('.jar')},
            'recipe': {str(path.relative_to(ROOT)): digest(path) for path in (
                Path(__file__).resolve(), ROOT / 'experiments/native-linux/userspace/art-compiler-test.c')},
            'compiler_settings': {'filter': 'verify', 'single_image': True,
                                  'image_offset': 2097152, 'max_heap_mib': 256}}


def generate_header(path):
    entries = ',\n'.join('{' + ','.join(json.dumps(item[key]) for key in ('name', 'dex', 'context')) + '}'
                         for item in DESKTOP)
    path.write_text('static const struct { const char *name, *dex, *context; } '
                    'aot_desktop_targets[] = {\n' + entries + '\n};\n')


def code_ranges(data, section_offset, section_address, ranges):
    """按固定版本 VIXL 的池标记划分 NCE OAT 方法；原生 ELF 不使用此规则。"""
    result = []
    for begin, end in ranges:
        if begin % 4 or end % 4 or begin >= end:
            raise RuntimeError('Invalid OAT method instruction bounds')
        code_begin = address = begin
        while address < end:
            offset = section_offset + address - section_address
            word, = struct.unpack_from('<I', data, offset)
            if word & 0xff00001f != 0x5800001f:
                address += 4
                continue
            # LiteralPool::Emit encodes the entire pool length, including its marker.
            words = (word >> 5) & 0x7ffff
            pool_end = address + words * 4
            if words < 2 or words >= 0x40000 or pool_end > end:
                raise RuntimeError('Invalid VIXL literal pool bounds')
            previous = struct.unpack_from('<I', data, offset - 4)[0] if address > code_begin else 0
            # ART finalizes methods as unreachable. A throwing slow path may end
            # in BL; the compiler's pool marker supplies its data boundary.
            terminal_call = pool_end == end and previous & 0xfc000000 == 0x94000000
            if not terminal_call and previous & 0xfc000000 != 0x14000000 and previous & 0xfffffc1f not in (0xd61f0000, 0xd65f0000):
                raise RuntimeError(f'VIXL literal pool has a reachable fallthrough at {address:x}: previous={previous:08x}')
            if code_begin < address:
                result.append((code_begin, address))
            code_begin = address = pool_end
        if code_begin < end:
            result.append((code_begin, end))
    return result
