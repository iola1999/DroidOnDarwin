#!/usr/bin/env python3
"""从固定 HyperOS-AVD 发布输入准备原 init 清单、元数据和只读文件。"""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import struct
import subprocess
import tarfile
import zipfile

from build_apk_fixture import elf_loads
from hyperos_erofs_metadata import DUMP, ErofsMetadata
from hyperos_ramdisk import extract_ramdisk
from inspect_hyperos_files import DEBUGFS, READELF, elf_layout
from inspect_hyperos_input import MANIFEST_SHA256, ROOT, copy_member, digest, require
from stock_init_metadata import read_partition_metadata

SCAN_SHA256 = 'bfa0564789255476b36d361810ee23f42b5b99ffae77eb6743778377850bc370'
SYSTEM_SHA256 = 'dd65f33741bfce77b22b5c58b8d696b16da52f15a825acafcbcede47a3cfa3ca'
VENDOR_SHA256 = '0549061f9afe13c94baaa942eb08319b0498b66ac38b2cfd73cbbba541153fbd'
SOURCE_COMMIT = '9d7ddd3f046e89d33cb205b5fbbf6dc5b728155c'
CONFIG_PREFIXES = tuple('/' + part + '/etc/' + directory + '/'
                        for part in ('system', 'system_ext', 'product', 'vendor')
                        for directory in ('init', 'selinux'))


def elf_info(data):
    kind, machine = struct.unpack_from('<HH', data, 16)
    basic = {'type': kind, 'machine': machine, 'bits': data[4] * 32}
    if kind not in (2, 3):
        return {'elf': basic, 'execution_supported_by_current_runner': False}
    layout = dict(elf_layout(data), type=kind)
    congruent = bool(layout['loads']) and all((s['address'] - s['offset']) % 4096 == 0
                                            for s in layout['loads'])
    supported = layout['bits'] == 64 and layout['machine'] == 183
    result = {'elf': layout, 'guest_4k_address_congruent': congruent,
              'execution_supported_by_current_runner': supported}
    if supported:
        try:
            elf_loads(data, page_bytes=4096)
            result['guest_4k_load_valid'] = True
        except ValueError as error:
            result.update(guest_4k_load_valid=False, load_error=str(error))
    return result


def rc_records(files):
    services, actions, imports = [], [], []
    for guest, record in files.items():
        if not guest.endswith('.rc'):
            continue
        current = None
        lines = []
        pending, first = '', 0
        for number, raw in enumerate(Path(record['source']).read_text(errors='replace').splitlines(), 1):
            if not pending:
                first = number
            pending += raw
            if pending.rstrip().endswith('\\'):
                pending = pending.rstrip()[:-1] + ' '
                continue
            lines.append((first, pending))
            pending = ''
        require(not pending, '原 rc 续行未结束：' + guest)
        for number, line in lines:
            fields = line.split('#', 1)[0].split()
            if not fields:
                continue
            if not line[0].isspace():
                current = None
                if len(fields) >= 3 and fields[0] == 'service':
                    current = {'rc': guest, 'line': number, 'name': fields[1], 'binary': fields[2],
                               'arguments': fields[3:], 'declaration': line, 'options': [],
                               'classes': ['default'], 'disabled': False}
                    services.append(current)
                elif fields[0] == 'on':
                    current = {'rc': guest, 'line': number, 'trigger': line.strip(), 'commands': []}
                    actions.append(current)
                elif fields[0] == 'import':
                    imports.append({'rc': guest, 'line': number, 'text': line.strip()})
            elif current is not None:
                if 'options' in current:
                    current['options'].append({'line': number, 'text': line.strip()})
                    if fields[0] == 'class':
                        current['classes'] = fields[1:]
                    elif fields[0] == 'disabled':
                        current['disabled'] = True
                else:
                    current['commands'].append({'line': number, 'text': line.strip()})
    for service in services:
        matches = []
        for action in actions:
            commands = []
            for command in action['commands']:
                fields = command['text'].split()
                if len(fields) >= 2 and ((fields[0] in ('start', 'stop', 'restart', 'enable', 'exec_start') and fields[1] == service['name']) or
                                         (fields[0].startswith('class_') and fields[1] in service['classes'])):
                    commands.append(command)
            if commands:
                matches.append({k: action[k] for k in ('rc', 'line', 'trigger')} | {'commands': commands})
        service['declared_actions'] = matches
    return services, imports


def archive_inputs(parts, manifest, out):
    selected = {'images/ramdisk.img', 'images/kernel-ranchu', 'images/system.img'}
    result = {}
    process = subprocess.Popen(['/bin/cat', *map(str, parts)], stdout=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=process.stdout, mode='r|gz') as archive:
            seen = set()
            for member in archive:
                name = PurePosixPath(member.name)
                require(not name.is_absolute() and '..' not in name.parts and member.name not in seen,
                        '原发布包归档路径无效或重复')
                seen.add(member.name)
                if member.name not in selected:
                    continue
                info = manifest['files'][member.name]
                require(member.isfile() and member.size == info['size'], '原归档成员类型或大小不同')
                partition = member.name == 'images/system.img'
                target = out / ('system_dlkm.erofs' if partition else name.name)
                result[member.name] = copy_member(archive.extractfile(member), target, info, partition,
                                                  partition='system_dlkm')
                print(json.dumps({'archive_input': target.name, 'bytes': target.stat().st_size}), flush=True)
                if set(result) == selected:
                    break
        require(set(result) == selected, '原发布包缺少 ramdisk、kernel 或 system_dlkm')
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=10)
    return result


def prepare(release, out):
    manifest_path, scan_path = release / 'manifest.json', release / 'core-scan-v4/scan.json'
    require(digest(manifest_path) == MANIFEST_SHA256 and digest(scan_path) == SCAN_SHA256,
            '固定发布或扫描清单摘要不匹配')
    manifest, scan = [json.loads(p.read_text()) for p in (manifest_path, scan_path)]
    parts = [release / row['name'] for row in manifest['parts']]
    for path, row in zip(parts, manifest['parts']):
        require(path.stat().st_size == row['size'] and digest(path) == row['sha256'], '原分卷摘要不匹配：' + str(path))
    system, vendor = release / 'extracted/system.ext4', release / 'logical-vendor/vendor.erofs'
    for image, expected in ((system, SYSTEM_SHA256), (vendor, VENDOR_SHA256)):
        require(digest(image) == expected, '原分区摘要不匹配：' + str(image))
    require(scan['passed'] and scan['inputs']['system.ext4']['sha256'] == SYSTEM_SHA256 and
            scan['inputs']['vendor.erofs']['sha256'] == VENDOR_SHA256, '扫描记录与固定分区不同')
    out.mkdir(parents=True, exist_ok=False)
    (out / 'logs').mkdir()
    commands, files, links = [], {}, {}

    def run(command, binary=False):
        result = subprocess.run(list(map(str, command)), capture_output=True, timeout=120)
        log = out / 'logs' / f'{len(commands):04d}.txt'
        log.write_bytes(result.stderr if binary else result.stdout + b'\n[stderr]\n' + result.stderr)
        commands.append({'command': list(map(str, command)), 'returncode': result.returncode,
                         'log': str(log.relative_to(out)), 'log_sha256': digest(log)})
        return result

    raw = archive_inputs(parts, manifest, out)
    ramdisk = extract_ramdisk(out / 'ramdisk.img', out)
    for entry in ramdisk['entries']:
        if entry.get('extracted') and Path(entry['extracted']).read_bytes().startswith(b'\x7fELF'):
            entry.update(elf_info(Path(entry['extracted']).read_bytes()))
    identity = {'manifest': str(manifest_path), 'manifest_sha256': MANIFEST_SHA256,
                'scan': str(scan_path), 'scan_sha256': SCAN_SHA256,
                'repository': 'NEORUAA/HyperOS-AVD', 'source_commit': SOURCE_COMMIT,
                'release': 'v0.1.0-a16-hyperos3-fuxi-r1', 'hyperos': manifest['hyperos'],
                'source': 'Third-party MysticGSI assembly of HyperOS system and ranchu vendor/system_dlkm',
                'parts': manifest['parts'], 'archive_inputs': raw}
    (out / 'source-identity.json').write_text(json.dumps(identity, ensure_ascii=False, indent=2) + '\n')
    images = {'/': system, '/vendor': vendor}
    erofs = {vendor: ErofsMetadata(vendor, run)}
    apex = []
    for entry in scan['inventory']['/system/apex']:
        guest = entry['path']
        if not guest.endswith(('.apex', '.capex')):
            continue
        result = run([DEBUGFS, '-R', 'cat ' + guest, system], binary=True)
        require(result.returncode == 0 and len(result.stdout) == entry['bytes'], '原 APEX 内容读取失败：' + guest)
        target = out / 'apex' / Path(guest).name
        target.parent.mkdir(exist_ok=True)
        target.write_bytes(result.stdout)
        row = {'path': guest, 'source': str(target), 'source_image': str(system),
               'source_image_path': guest, 'bytes': target.stat().st_size, 'sha256': digest(target)}
        contents = out / 'apex' / (target.name + '.contents')
        contents.mkdir()
        data = result.stdout
        with zipfile.ZipFile(io.BytesIO(data)) as package:
            if 'original_apex' in package.namelist():
                data = package.read('original_apex')
                original = contents / 'original.apex'
                original.write_bytes(data)
                row.update(original_apex=str(original), original_apex_sha256=digest(original))
        with zipfile.ZipFile(io.BytesIO(data)) as package:
            members = {}
            for name in ('apex_manifest.pb', 'apex_manifest.json', 'apex_pubkey', 'apex_payload.img'):
                if name not in package.namelist():
                    continue
                path = contents / name
                path.write_bytes(package.read(name))
                members[name] = {'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest(path)}
            decoded = subprocess.run(['/opt/homebrew/bin/protoc', '--decode_raw'],
                                     input=(contents / 'apex_manifest.pb').read_bytes(), capture_output=True,
                                     check=True, timeout=30).stdout.decode()
        match = re.search(r'^1: "([^"\n]+)"$', decoded, re.MULTILINE)
        require(match is not None, '原 APEX manifest 缺少名称')
        name = match[1]
        prefix = '/apex/' + name
        require(prefix not in images, '原 APEX 名称重复')
        payload = contents / 'apex_payload.img'
        with payload.open('rb') as stream:
            header = stream.read(2048)
        payload_format = 'erofs' if header[1024:1028] == bytes.fromhex('e2e1f5e0') else 'ext4'
        require(payload_format == 'erofs' or header[1080:1082] == b'\x53\xef', '未知原 APEX payload 格式')
        images[prefix] = payload
        if payload_format == 'erofs':
            erofs[payload] = ErofsMetadata(payload, run)
        row.update(name=name, manifest_decoded=decoded, members=members, payload_format=payload_format,
                   activation_observed=False)
        apex.append(row)

    def location(guest):
        require(re.fullmatch(r'/[A-Za-z0-9_./@+\-:\[\]=%,]*', guest) and '..' not in PurePosixPath(guest).parts,
                '镜像路径无效：' + guest)
        prefix = next(p for p in sorted(images, key=len, reverse=True)
                      if p == '/' or guest == p or guest.startswith(p + '/'))
        path = (guest.removeprefix(prefix) or '/') if prefix != '/' else guest
        return images[prefix], path, prefix

    def fetch(guest, resolving=()):
        if guest in files:
            return files[guest]
        require(guest not in resolving, '原符号链接循环：' + guest)
        image, path, prefix = location(guest)
        original_link = None
        if image in erofs:
            meta = erofs[image].read(path)
            if meta is None:
                return None
            result = run([DUMP, '--cat', '--path=' + path, image], binary=True)
            require(result.returncode == 0, 'EROFS 文件读取失败：' + guest)
            data = result.stdout
            if meta['inode_type'] == 0o120000:
                original_link = dict(meta, link_target=data.decode())
            else:
                require(meta['inode_type'] == 0o100000, '原路径要求普通文件：' + guest)
        else:
            result = run([DEBUGFS, '-R', 'stat ' + path, image])
            stat_text = result.stdout.decode()
            match = re.search(r'Inode: (\d+)\s+Type: (\S+)\s+Mode:\s+([0-7]+)', stat_text)
            if not match:
                require(b'File not found' in result.stderr, 'ext4 路径检查失败：' + guest)
                return None
            require(result.returncode == 0, 'ext4 inode 读取失败：' + guest)
            if match[2] == 'symlink':
                original_link = read_partition_metadata(image, {}, [path], links={path})[path]
            else:
                require(match[2] == 'regular', '原路径要求普通文件：' + guest)
                result = run([DEBUGFS, '-R', 'cat ' + path, image], binary=True)
                require(result.returncode == 0, 'ext4 文件读取失败：' + guest)
                data = result.stdout
                expected_size = int(re.search(r'Size:\s+(\d+)', stat_text)[1])
                require(len(data) == expected_size, '原 ext4 文件大小不同：' + guest)
                meta = None
        target = out / ('apex-files' if prefix.startswith('/apex/') else 'root') / guest.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        if original_link:
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(guest), original_link['link_target']))
            child = fetch(resolved, (*resolving, guest))
            links[guest] = dict(original_link, source_image=str(image), source_image_path=path)
            if child is None:
                return None
            shutil.copyfile(child['source'], target)
            record = {k: child[k] for k in ('bytes', 'sha256', 'mode') if k in child}
            record.update(source=str(target), source_image=str(image), source_image_path=path,
                          source_link_target=original_link['link_target'], resolved_path=resolved,
                          staged_as='regular-file', original_link_metadata=original_link, metadata_follows_link=True)
        else:
            target.write_bytes(data)
            record = {'source': str(target), 'source_image': str(image), 'source_image_path': path,
                      'bytes': len(data), 'sha256': digest(target)}
            if meta is not None:
                record['original_metadata'] = meta
        data = target.read_bytes()
        if data.startswith(b'\x7fELF'):
            record.update(elf_info(data))
            result = run([READELF, '-l', '-d', target])
            require(result.returncode == 0, 'ELF 动态依赖读取失败：' + guest)
            text = result.stdout.decode()
            record['needed'] = re.findall(r'\(NEEDED\).*?\[([^]]+)\]', text)
            match = re.search(r'Requesting program interpreter: ([^]]+)', text)
            record['interpreter'] = match[1] if match else None
        files[guest] = record
        return record

    selected = {'/init.environ.rc', '/system/build.prop', '/system/etc/cgroups.json', '/system/etc/task_profiles.json',
                '/system_ext/etc/build.prop', '/product/etc/build.prop', '/vendor/build.prop', '/vendor/etc/fstab.ranchu'}
    selected.update(path for path in scan['files'] if path.startswith(CONFIG_PREFIXES))
    for guest in sorted(selected):
        require(fetch(guest) is not None, '原配置文件缺失：' + guest)
    services, imports = rc_records(files)
    require(len(services) == 179, '固定原 rc 服务声明数量不同')
    linkerconfig = '/apex/com.android.runtime/bin/linkerconfig'
    todo = ['/system/bin/init', '/system/bin/bootstrap/linker64', '/system/bin/ueventd', '/system/bin/apexd',
            '/system/bin/secilc', linkerconfig]
    closure, unresolved = set(), []
    while todo:
        guest = todo.pop(0)
        if guest in closure:
            continue
        closure.add(guest)
        record = fetch(guest)
        require(record is not None, '原启动依赖缺失：' + guest)
        for name in record.get('needed', []):
            roots = ['/system/lib64/bootstrap/', '/system/lib64/']
            if guest.startswith('/apex/com.android.runtime/'):
                roots = ['/apex/com.android.runtime/lib64/bionic/', '/apex/com.android.runtime/lib64/', *roots]
            candidate = next((root + name for root in roots if fetch(root + name)), None)
            if candidate is None:
                unresolved.append({'caller': guest, 'needed': name})
            else:
                todo.append(candidate)
    for service in services:
        binary = service['binary']
        candidate = '/' + binary if not binary.startswith('/') and '$' not in binary else binary
        record = fetch(candidate) if '$' not in candidate else None
        service.update(binary_present=bool(record), static_candidate=candidate,
                       relative_path_resolution_unverified=not binary.startswith('/'),
                       apex_activation_required=binary.startswith('/apex/'))
        if record:
            service['needed'] = record.get('needed', [])
            service['interpreter'] = record.get('interpreter')
            service['execution_supported_by_current_runner'] = record.get('execution_supported_by_current_runner')
            if record.get('elf', {}).get('bits') == 32:
                dependencies = []
                for name in record['needed']:
                    roots = ['/system/lib/bootstrap', '/system/lib', '/system_ext/lib', '/product/lib', '/vendor/lib']
                    roots += [prefix + directory for prefix in images if prefix.startswith('/apex/')
                              for directory in ('/lib', '/lib/bionic')]
                    candidates = [root + '/' + name for root in roots if fetch(root + '/' + name)]
                    dependencies.append({'needed': name, 'present': bool(candidates), 'candidates': candidates,
                                         'all_candidates_aarch32': all(files[p].get('elf', {}).get('bits') == 32 and
                                                                      files[p]['elf']['machine'] == 40 for p in candidates)})
                service['aarch32_dependency_presence'] = dependencies
                service['interpreter_present'] = bool(fetch(record['interpreter'])) if record['interpreter'] else None
                service['dependency_namespace_resolution_observed'] = False

    groups = {}
    for guest, record in files.items():
        if 'source_link_target' not in record:
            image, path, prefix = location(guest)
            if image not in erofs:
                groups.setdefault(image, {})[path] = record
    groups.setdefault(system, {}).update({row['path']: row for row in apex})
    directories = {}
    for image, records in groups.items():
        paths = set(records)
        for path in records:
            paths.update(str(parent) for parent in PurePosixPath(path).parents)
        metadata = read_partition_metadata(image, records, paths)
        for path, record in records.items():
            record['original_metadata'] = metadata[path]
        directories[str(image)] = {path: row for path, row in metadata.items() if path not in records}
    for row in apex:
        payload = Path(row['members']['apex_payload.img']['path'])
        row['payload_root_metadata'] = (erofs[payload].read('/') if payload in erofs else
                                        read_partition_metadata(payload, {}, ['/'])['/'])
        Path(row['source']).chmod(row['original_metadata']['mode'])
    for guest in files:
        record = files[guest]
        if 'resolved_path' in record:
            resolved = record['resolved_path']
            while 'resolved_path' in files[resolved]:
                resolved = files[resolved]['resolved_path']
            record['original_metadata'] = files[resolved]['original_metadata']
        record['mode'] = record['original_metadata']['mode']
        Path(record['source']).chmod(record['mode'])
    root_link_paths = [row['path'] for row in scan['inventory']['/'] if row['mode'] & 0o170000 == 0o120000]
    root_links = read_partition_metadata(system, {}, root_link_paths, links=set(root_link_paths))
    vendor_dirs = {str(p) for guest in files if guest.startswith('/vendor/') for p in PurePosixPath(guest).parents
                   if str(p) == '/vendor' or str(p).startswith('/vendor/')}
    vendor_metadata = {guest: erofs[vendor].read(guest.removeprefix('/vendor') or '/') for guest in sorted(vendor_dirs)}
    compatibility = {}
    for prefix, part in (('plat', 'system'), ('system_ext', 'system_ext'), ('product', 'product')):
        original = Path(files[f'/{part}/etc/selinux/{prefix}_sepolicy_and_mapping.sha256']['source']).read_bytes()
        precompiled = Path(files[f'/vendor/etc/selinux/precompiled_sepolicy.{prefix}_sepolicy_and_mapping.sha256']['source']).read_bytes()
        compatibility[prefix] = {'system_hash': original.decode().strip(), 'vendor_precompiled_hash': precompiled.decode().strip(),
                                 'matches': original == precompiled}
    version = Path(files['/vendor/etc/selinux/plat_sepolicy_vers.txt']['source']).read_text().strip()
    policy = {'vendor_platform_policy_version': version, 'precompiled_hashes': compatibility,
              'system_mapping_files': [row['path'] for row in scan['inventory']['/system/etc/selinux/mapping']],
              'requested_system_mapping_present': f'/system/etc/selinux/mapping/{version}.cil' in files,
              'selinux_setup_observed': False}
    root_files = {g: r for g, r in files.items() if not g.startswith('/apex/')}
    apex_files = {g: r for g, r in files.items() if g.startswith('/apex/')}
    dirs = {str(p) for guest in root_files for p in PurePosixPath(guest).parents if str(p) != '/'}
    lines = ['D\t' + p for p in sorted(dirs, key=lambda p: (p.count('/'), p))]
    lines += ['F\t%o\t%s' % (r['mode'], g) for g, r in sorted(root_files.items())]
    link = root_links['/etc']
    lines.append('L\t%d\t%d\t/etc\t%s' % (link['uid'], link['gid'], link['link_target']))
    (out / 'files.tsv').write_text('\n'.join(lines) + '\n')
    report = {'scope': 'Fixed original HyperOS init input preparation; no init or APEX activation execution',
              'source_identity': identity, 'source_identity_sha256': digest(out / 'source-identity.json'),
              'images': {name: {'path': str(image), 'bytes': image.stat().st_size, 'sha256': digest(image)}
                         for name, image in (('system', system), ('vendor', vendor), ('system_dlkm', out / 'system_dlkm.erofs'))},
              'system_dlkm_root_metadata': ErofsMetadata(out / 'system_dlkm.erofs', run).read('/'),
              'files': root_files, 'apex_files': apex_files, 'directories_metadata': directories,
              'vendor_directories_metadata': vendor_metadata,
              'unresolved_symbolic_links': {guest: row for guest, row in links.items() if guest not in files},
              'root_links': root_links,
              'bootstrap_closure': {'files': sorted(path for path in closure if not path.startswith('/apex/')), 'unresolved': unresolved},
              'activation_dependent_closure': {'files': sorted(path for path in closure if path.startswith('/apex/')),
                                               'activation_observed': False},
              'init_helper_paths': {'linkerconfig': linkerconfig, 'secilc': '/system/bin/secilc'},
              'rc_imports': imports, 'services': services,
              'apex': apex, 'ramdisk': ramdisk, 'selinux_compatibility': policy, 'commands': commands,
              'missing_service_binaries': [service['binary'] for service in services if not service['binary_present']],
              'files_tsv_sha256': digest(out / 'files.tsv'),
              'reader_sources': {name: digest(ROOT / 'scripts' / name) for name in
                                 ('prepare_hyperos_init.py', 'hyperos_erofs_metadata.py', 'hyperos_ramdisk.py',
                                  'stock_init_metadata.py', 'inspect_hyperos_input.py', 'inspect_hyperos_files.py', 'build_apk_fixture.py')},
              'limitations': ['Static AArch32 library presence does not validate linker namespaces or execution.',
                              'Original APEX container bytes and payload samples retained; activation not executed.',
                              'Legacy files.tsv records staging modes; raw mounts are needed to retain original filesystem labels and owners.',
                              'Root symlink modes, first-stage ramdisk/modules/devices and full enforcing init remain untested.'],
              'prepared': not unresolved}
    (out / 'manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'manifest': str(out / 'manifest.json'), 'prepared': report['prepared'],
                      'files': len(root_files), 'apex_files': len(apex_files), 'services': len(services),
                      'apex': len(apex), 'bootstrap_files': len(closure), 'unresolved': unresolved}), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    require(out.is_relative_to(ROOT / 'work/native-linux-nce'), '输出必须位于本工作树 work/native-linux-nce')
    require(not out.exists(), '输出目录已存在：' + str(out))
    try:
        prepare(args.release_directory.resolve(), out)
    except Exception as error:
        if out.is_dir():
            failure = {'scope': 'Fixed original HyperOS input preparation stopped', 'error': str(error),
                       'release_directory': str(args.release_directory.resolve()),
                       'generator_sha256': digest(Path(__file__).resolve())}
            (out / 'failure.json').write_text(json.dumps(failure, ensure_ascii=False, indent=2) + '\n')
        raise


if __name__ == '__main__':
    main()
