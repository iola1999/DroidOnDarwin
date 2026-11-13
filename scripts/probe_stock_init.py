#!/usr/bin/env python3
"""在已验证的 sparse 构建上观察固定原始 init 的启动与 PID 1 回收。"""
import argparse
import hashlib
import json
import os
from datetime import datetime
from pathlib import Path
import re
import subprocess
from probe_lkl_translation import validate_guest_layout

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
RUNTIME_SHA = '4b1a45b0ee51725c68f1c29765958d0b60b45a5f2325bedfc61764cd49232249'
DESKTOP_SHA = '515c9fe37032c16e01048a9a63f83708b314558d5322de2eb41773db09179772'
SYSTEM_IMAGE_SHA = '54a96f82a138e0919686cefffead8861e10c6189bf94188fef4fa08e24dc3ed1'
ROOT_FILES = {'/init.environ.rc': '460eecf019489a02ad7be82a9cc2234784f561b644f8f2bf1741e43514593060',
              '/system/build.prop': 'e7ece731169cc37c759dd827c206a0c3c3c24fde6937dd96982a4b83d70dfc66'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_observations(path):
    rows = []
    with path.open() as source:
        for number, line in enumerate(source, 1):
            if line.startswith('{'):
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as error:
                    raise ValueError(f'观察日志解析失败：{path}:{number}:{error.colno}: {error.msg}') from error
    return rows


def _first_stage_runner_source(source):
    replacements = (
        ('int (*start)(unsigned long, unsigned long *, unsigned) = (void *)(kernel + NCE_LKL_START_CPUS);',
         'int (*start)(unsigned long, unsigned long *, unsigned, const char *) = (void *)(kernel + NCE_LKL_START_CPUS_CMDLINE);'),
        ('start(scheduler_slice_us * 1000, &slice, online_cpus)',
         'start(scheduler_slice_us * 1000, &slice, online_cpus, stock_init_first_stage_enabled() ? "androidboot.hardware=ranchu" : NULL)'),
        ('long random[6] = {-100, (long)"/dev/random", 2};',
         'long random[6] = {-100, (long)(stock_init_first_stage_enabled() ? "/dev/urandom" : "/dev/random"), 2};'),
        ('static void seed_entropy(void)',
         'static int stock_init_first_stage_enabled(void);\n\nstatic void seed_entropy(void)'),
    )
    if any(source.count(original) != 1 for original, _ in replacements):
        raise ValueError('运行器启动参数或熵来源位置已变化')
    for original, replacement in replacements:
        source = source.replace(original, replacement)
    return source


def prepare(runtime, desktop, metadata, out, system_image=None, *, sdk_partitions=False):
    if sdk_partitions and not system_image:
        raise ValueError('完整 SDK36 LP 需要原始系统镜像')
    if system_image and sha(system_image) != SYSTEM_IMAGE_SHA:
        raise ValueError('SDK36 原始系统镜像摘要不匹配')
    runtime_manifest = runtime.with_name('android36-runtime.json')
    desktop_manifest = desktop / 'manifest.json'
    if sha(runtime_manifest) != RUNTIME_SHA or sha(desktop_manifest) != DESKTOP_SHA:
        raise ValueError('SDK36 固定输入清单不匹配')
    manifests = {runtime: json.loads(runtime_manifest.read_text()), desktop: json.loads(desktop_manifest.read_text())}
    records = {}

    def add(source, relative, guest):
        if guest in records:
            return
        path = source / relative
        canonical = str(path.resolve().relative_to(source)) if path.is_symlink() else relative
        expected = ROOT_FILES[guest] if source == metadata else manifests[source]['files'][canonical]
        if sha(path) != expected:
            raise ValueError('原始文件摘要不匹配：' + str(path))
        target = out / 'root' / guest.lstrip('/')
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cp', '-c', path, target], check=True, timeout=30)
        mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
        target.chmod(mode)
        records[guest] = {'source': str(path), 'sha256': expected, 'bytes': path.stat().st_size, 'mode': mode}
        records[guest].update(staged_path=str(target), staging='apfs-clone')
        if path.is_symlink():
            records[guest].update(source_link_target=str(path.readlink()), verified_target=canonical, staged_as='regular-file')

    todo, scanned = ['bin/init', 'bin/bootstrap/linker64'], set()
    while todo:
        relative = todo.pop(0)
        if relative in scanned:
            continue
        scanned.add(relative)
        add(runtime, relative, '/system/' + relative)
        dynamic = subprocess.check_output(['/opt/homebrew/opt/llvm/bin/llvm-readelf', '-d', runtime / relative], text=True)
        for library in re.findall(r'\(NEEDED\).*\[([^\]]+)\]', dynamic):
            candidate = next((name for name in ('lib64/bootstrap/' + library, 'lib64/' + library) if (runtime / name).is_file()), None)
            if candidate is None:
                raise ValueError('缺少 DT_NEEDED：' + library)
            todo.append(candidate)
    closure = {'files': len(scanned), 'bytes': sum(records['/system/' + name]['bytes'] for name in scanned)}
    for source, prefix, directories in ((runtime, '/system/', ('etc/init', 'etc/selinux')),
                                         (desktop, '/', ('vendor/etc/init', 'vendor/etc/selinux', 'system_ext/etc/init', 'system_ext/etc/selinux', 'product/etc/init', 'product/etc/selinux'))):
        for directory in directories:
            for path in sorted((source / directory).rglob('*')):
                if path.is_file():
                    relative = str(path.relative_to(source))
                    add(source, relative, prefix + relative)
    for relative in ('bin/ueventd', 'etc/cgroups.json', 'etc/task_profiles.json'):
        add(runtime, relative, '/system/' + relative)
    for relative in ('vendor/etc/fstab.ranchu', 'system_ext/etc/build.prop', 'product/etc/build.prop'):
        if (desktop / relative).is_file():
            add(desktop, relative, '/' + relative)
    for guest in ROOT_FILES:
        add(metadata, guest.lstrip('/'), guest)
    if sdk_partitions:
        from stock_init_sdk import SERVICE_FILES
        add(runtime, 'bin/keystore2', '/system/bin/keystore2')
        for guest in SERVICE_FILES:
            add(desktop, guest.lstrip('/'), guest)
    directories = {str(parent) for guest in records for parent in Path(guest).parents if str(parent) != '/'}
    lines = ['D\t' + name for name in sorted(directories, key=lambda name: (name.count('/'), name))]
    lines += ['F\t%o\t%s' % (item['mode'], guest) for guest, item in records.items()]
    original_metadata = None
    sdk_input = {}
    if system_image:
        if sdk_partitions:
            from stock_init_sdk import prepare_partitions
            sdk_input = prepare_partitions(system_image, records, out)
            for path, link in sorted(sdk_input['original_root_links'].items()):
                lines.append('L\t%d\t%d\t%s\t%s\t%s' %
                             (link['uid'], link['gid'], path, link['link_target'], link['selinux']))
        else:
            from stock_init_metadata import metadata as read_metadata
            original_metadata = read_metadata(system_image, records, directories, out)
            link = original_metadata['/etc']
            lines.append('L\t%d\t%d\t/etc\t%s' % (link['uid'], link['gid'], link['link_target']))
    (out / 'files.tsv').write_text('\n'.join(lines) + '\n')

    deployed = ({'path': str(system_image), 'sha256': SYSTEM_IMAGE_SHA} if sdk_partitions else
                {'path': str(out / 'original-system-metadata.img'), 'sha256': sha(out / 'original-system-metadata.img')} if system_image else None)
    return {**sdk_input, 'runtime_manifest': str(runtime_manifest), 'runtime_manifest_sha256': RUNTIME_SHA,
            'desktop_manifest': str(desktop_manifest), 'desktop_manifest_sha256': DESKTOP_SHA,
            'closure': closure, 'files': records, 'original_system_metadata': original_metadata,
            'original_system_image': {'path': str(system_image), 'sha256': SYSTEM_IMAGE_SHA} if system_image else None,
            'deployed_system_partition': deployed,
            'limitations': [('Original ext4 system/vendor/product/system_ext and EROFS system_dlkm extents mounted read-only.' if sdk_partitions else
                              'Original system partition mounted read-only; vendor/product/system_ext service binaries are not deployed.' if system_image else
                              'Original rc files retained; service binaries beyond init/ueventd and APEX containers are not staged.'),
                            'The runner supplies mounted partition directories; first-stage init and original fstab mounts are untested.']}


def validate(rows, mode, original_system=False, *, guest_page_bytes=16384, kernel_log='', original_input=None, exec_elf_images=None, first_stage=False):
    exec_contract = (exec_elf_images is not None or
                     (original_input is not None and 'exec_elf_images' in original_input))
    layouts = [row for row in rows if row.get('config') == 'translation_guest_layout']
    if len(layouts) != 1:
        return False
    guest = layouts[0]
    if original_input and not original_system:
        return False
    if 'BUG: non-zero pgtables_bytes on freeing mm:' in kernel_log:
        return False
    if not validate_guest_layout(rows, 'sparse', True, guest_page_bytes, sparse_end=guest.get('guest_end')):
        return False
    if original_input and 'hyperos_manifest' in original_input and (
            guest_page_bytes != 4096 or (mode != 'lifecycle' and guest['guest_end'] != 1 << 39)):
        return False
    observations = [r for r in rows if 'stock_init_observation' in r]
    cleanup = [r for r in rows if r.get('test') == 'translated_context_cleanup']
    logs = [r for r in rows if 'stock_init_kernel_log' in r]
    endings = [r for r in rows if 'result' in r and 'component_vm' not in r and 'stock_init_write' not in r and 'stock_init_security' not in r and 'stock_init_syscall' not in r and 'stock_init_block' not in r and 'stock_init_stop_request' not in r and 'stock_init_host_stop' not in r and 'stock_init_host_stop_reaped' not in r and 'stock_init_metadata_directory_sync' not in r and 'stock_init_dm_table_load' not in r and 'stock_init_ramdisk_mknod' not in r and not any(key.startswith('stock_init_first_stage_') for key in r)]
    if len(observations) != 1 or len(cleanup) != 1 or len(logs) != 1 or endings != [{'result': 'passed'}]:
        return False
    o, c, log = observations[0], cleanup[0], logs[0]
    if (set(o) != {'stock_init_observation', 'mode', 'ancestor_pid', 'waited', 'status', 'deadline_stop', 'kill_result'} or
            o['stock_init_observation'] is not True or o['mode'] != mode or type(o['deadline_stop']) is not bool or
            any(type(o[key]) is not int for key in ('ancestor_pid', 'waited', 'status', 'kill_result')) or
            o['ancestor_pid'] <= 1 or o['waited'] != o['ancestor_pid'] or not 0 <= o['status'] <= 65535 or
            (o['deadline_stop'] and o['status'] != 9) or
            c.get('passed') is not True or type(c.get('allocated')) is not int or c['allocated'] <= 0 or
            type(c.get('freed')) is not int or c['allocated'] != c['freed'] or
            (exec_contract and (type(c.get('stock_init_exec_count')) is not int or
                                not 0 <= c['stock_init_exec_count'] < 1 << 64)) or
            set(log) != {'stock_init_kernel_log', 'read_result'} or log['stock_init_kernel_log'] is not True or
            type(log['read_result']) is not int or not -4095 <= log['read_result'] < 1 << 20):
        return False
    if exec_contract and [row for row in rows if 'stock_init_observation' in row or
                          row.get('test') == 'translated_context_cleanup' or
                          row.get('result') in ('passed', 'failed')] != [o, c, {'result': 'passed'}]:
        return False
    from stock_init_stop import validate as validate_stop
    stop_contract = (original_input or {}).get('namespace_stop_contract')
    if stop_contract is not None and (type(stop_contract) is not int or stop_contract != 1):
        return False
    if not validate_stop(rows, o, mode, required=stop_contract == 1):
        return False
    writes = [r for r in rows if 'stock_init_write' in r]
    for row in writes:
        if (set(row) != {'stock_init_write', 'fd', 'result', 'vector', 'bytes', 'captured', 'hex'} or
                any(type(row[key]) is not int for key in ('stock_init_write', 'fd', 'result', 'vector', 'bytes', 'captured')) or
                row['stock_init_write'] not in (64, 66) or row['fd'] < 0 or not 0 <= row['vector'] < 8 or
                not 0 < row['captured'] <= min(row['bytes'], 2048) or type(row['hex']) is not str or
                re.fullmatch('[0-9a-f]{%d}' % (2 * row['captured']), row['hex']) is None):
            return False
    security = [r for r in rows if 'stock_init_security' in r]
    for row in security:
        init_paths = ('/init', '/system/bin/init') if first_stage else ('/system/bin/init',)
        init_exec = (exec_contract and row.get('stock_init_security') == 221 and row.get('result') == 0 and
                     row.get('path_hex') in {path.encode().hex() for path in init_paths})
        fields = {'stock_init_security', 'result', 'fd', 'path_hex', 'name_hex', 'value_hex'}
        if init_exec:
            fields.add('exec_index')
        if (set(row) != fields or
                any(type(row[key]) is not int for key in ('stock_init_security', 'result', 'fd')) or
                (init_exec and (type(row['exec_index']) is not int or not 0 < row['exec_index'] < 1 << 64)) or
                row['stock_init_security'] not in (*range(5, 11), 56, 221) or row['fd'] < -1 or
                any(type(row[key]) is not str or re.fullmatch('(?:[0-9a-f]{2})*', row[key]) is None
                    for key in ('path_hex', 'name_hex', 'value_hex')) or
                len(row['path_hex']) > 510 or len(row['name_hex']) > 254 or len(row['value_hex']) > 512):
            return False
    for row in (r for r in rows if 'stock_init_syscall' in r or 'stock_init_reboot' in r):
        reboot = 'stock_init_reboot' in row
        if (set(row) != ({'stock_init_reboot', 'args'} if reboot else {'stock_init_syscall', 'args', 'pc', 'result'}) or
                type(row['args']) is not list or len(row['args']) != (4 if reboot else 6) or
                any(type(value) is not int or not 0 <= value < 1 << 64 for value in row['args']) or
                (reboot and row['stock_init_reboot'] is not True) or
                (not reboot and (type(row['stock_init_syscall']) is not int or type(row['result']) is not int or
                                 type(row['pc']) is not int or not guest['guest_base'] <= row['pc'] < guest['guest_end']))):
            return False
    for row in (r for r in rows if 'stock_init_block' in r):
        counts = {29: 0, 33: 1, 40: 3, 56: 1}
        if (original_input or {}).get('first_stage_contract') == 1:
            counts.update({34: 1, 35: 1, 36: 2, 39: 1, 49: 1, 51: 1})
        if (set(row) != {'stock_init_block', 'pid', 'result', 'args', 'paths_hex'} or
                type(row['stock_init_block']) is not int or row['stock_init_block'] not in counts or
                type(row['pid']) is not int or row['pid'] <= 0 or type(row['result']) is not int or
                type(row['args']) is not list or len(row['args']) != 6 or
                any(type(value) is not int or not 0 <= value < 1 << 64 for value in row['args']) or
                type(row['paths_hex']) is not list or len(row['paths_hex']) != counts[row['stock_init_block']] or
                any(type(value) is not str or len(value) > 510 or re.fullmatch('(?:[0-9a-f]{2})*', value) is None
                    for value in row['paths_hex'])):
            return False
    contexts = [r for r in rows if 'stock_init_task_context' in r]
    for row in contexts:
        if (set(row) != {'stock_init_task_context', 'ancestor_pid', 'read_result', 'label_hex'} or
                row['stock_init_task_context'] is not True or type(row['ancestor_pid']) is not int or
                row['ancestor_pid'] != o['ancestor_pid'] or type(row['read_result']) is not int or
                not -4095 <= row['read_result'] <= 255 or type(row['label_hex']) is not str or
                re.fullmatch('[0-9a-f]{%d}' % (2 * max(row['read_result'], 0)), row['label_hex']) is None):
            return False
    if first_stage:
        from stock_init_first_stage import validate as validate_first_stage
        return validate_first_stage(rows, mode, original_input, guest, guest_page_bytes)
    if (original_input or {}).get('first_stage_contract') == 1:
        from stock_init_first_stage_devices import validate as validate_devices
        if not validate_devices(rows, original_input, False):
            return False
        devices = next(row for row in rows if 'stock_init_first_stage_devices' in row)
        mounted = next((row for row in rows if 'stock_init_original_system_mounted' in row), None)
        if mounted is None or rows.index(devices) >= rows.index(mounted):
            return False
    mounts = [r for r in rows if 'stock_init_original_system_mounted' in r]
    if (mounts != ([{'stock_init_original_system_mounted': True, 'readonly': True}] if original_system else []) or
            any(row['stock_init_original_system_mounted'] is not True or row['readonly'] is not True for row in mounts)):
        return False
    links = [r for r in rows if 'stock_init_root_symlink' in r]
    expected_links = original_input['original_root_links'] if original_input else {'/etc': {'link_target': '/system/etc', 'uid': 0, 'gid': 0}}
    if (links != ([{'stock_init_root_symlink': True, 'path': path, 'target': row['link_target'],
                  'uid': row['uid'], 'gid': row['gid'], 'creation_mode': 0o777}
                 for path, row in sorted(expected_links.items())] if original_system else []) or
            any(row['stock_init_root_symlink'] is not True or any(type(row[key]) is not int for key in ('uid', 'gid', 'creation_mode'))
                for row in links)):
        return False
    partitions = [r for r in rows if 'stock_init_original_partition_mounted' in r]
    expected_partitions = [('/product', 'bind'), ('/system_ext', 'bind'), ('/vendor', 'erofs'), ('/system_dlkm', 'erofs')]
    if original_input:
        expected_partitions = original_input.get('partition_mounts', expected_partitions)
    if (partitions != ([{'stock_init_original_partition_mounted': True, 'path': path, 'filesystem': filesystem, 'readonly': True}
                       for path, filesystem in expected_partitions] if original_input else []) or
            any(row['stock_init_original_partition_mounted'] is not True or row['readonly'] is not True for row in partitions)):
        return False
    root_files = [r for r in rows if 'stock_init_original_root_file' in r]
    if (root_files != ([{'stock_init_original_root_file': True, 'path': '/init.environ.rc', 'readonly': True}] if original_input else []) or
            any(row['stock_init_original_root_file'] is not True or row['readonly'] is not True for row in root_files)):
        return False
    metadata_rows = [r for r in rows if 'stock_init_original_metadata' in r]
    expected_metadata = [{'stock_init_original_metadata': True, 'path': path, 'uid': row['uid'], 'gid': row['gid'],
                          'mode': row['mode'], 'inode_type': row['inode_type'], 'label_hex': row['selinux_xattr_hex']}
                         for path, row in sorted(original_input['expected_runtime_metadata'].items())] if original_input else []
    if (metadata_rows != expected_metadata or any(row['stock_init_original_metadata'] is not True or
                                                 any(type(row[key]) is not int for key in ('uid', 'gid', 'mode', 'inode_type'))
                                                 for row in metadata_rows)):
        return False
    from stock_init_sdk import validate_dm
    if not validate_dm(rows, (original_input or {}).get('sdk_partitions'), mode=mode):
        return False
    from stock_init_data import validate as validate_data
    metadata_input = (original_input or {}).get('metadata_image')
    if not validate_data(rows, bool((original_input or {}).get('data_image')), mode, metadata_enabled=bool(metadata_input)):
        return False
    from stock_init_metadata_volume import validate as validate_metadata_volume
    if not validate_metadata_volume(rows, bool(metadata_input), mode, bool((original_input or {}).get('data_image')),
                                    (metadata_input or {}).get('partition')):
        return False
    lifecycle = [r for r in rows if 'stock_init_lifecycle' in r]
    original = [r for r in rows if 'stock_init_exec' in r]
    restorer = [r for r in rows if r.get('test') == 'linux_signal_kernel_restorer']
    if restorer != [{'test': 'linux_signal_kernel_restorer', 'passed': True, 'pid': pid}
                    for pid in ((1, 2) if mode == 'lifecycle' else (1,))]:
        return False
    if mode == 'lifecycle':
        return (lifecycle == [{'stock_init_lifecycle': True, 'pid': 1, 'child': 2}] and not original and
                o['deadline_stop'] and o['status'] == 9 and
                c['allocated'] == (3 if any('stock_init_owned_ordinary' in row for row in rows) else 2) and log['read_result'] >= 0 and
                (not exec_contract or (c['stock_init_exec_count'] == 0 and not any('exec_index' in row for row in security))) and
                not any('stock_init_exec_layout' in row for row in rows))
    if exec_contract:
        from exec_aslr import validate_exec_layout
        return (not lifecycle and original == [{'stock_init_exec': True, 'pid': 1, 'argument': 'selinux_setup'}] and
                validate_exec_layout(rows, exec_elf_images, guest, guest_page_bytes))
    if any('stock_init_exec_layout' in row for row in rows):
        return False
    paths = [r.get('path') for r in rows if r.get('phase') == 'runtime_elf']
    return (not lifecycle and original == [{'stock_init_exec': True, 'pid': 1, 'argument': 'selinux_setup'}] and
            paths.count('/system/bin/init') == 1 and paths.count('/system/bin/bootstrap/linker64') == 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--desktop', type=Path)
    parser.add_argument('--root-metadata', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--system-image', type=Path)
    parser.add_argument('--sdk-partitions', action='store_true', help='由固定原盘只读映射 SDK36 五个 LP 分区')
    parser.add_argument('--sdk-ramdisk', type=Path, help='无 stage 参数执行固定 SDK36 原 ramdisk 的 /init，并在同盘恢复检查')
    parser.add_argument('--hyperos-manifest', type=Path)
    parser.add_argument('--lifecycle-only', action='store_true', help='仅检查原分区元数据、PID 1 信号与停止回收')
    parser.add_argument('--data-image', type=Path, help='SDK36 mounted-partition 观察使用的显式 ext4 数据盘')
    parser.add_argument('--metadata-image', type=Path, help='SDK36 mounted-partition 观察使用的原 GPT metadata 盘独立可写副本')
    args = parser.parse_args()
    if args.metadata_image and args.hyperos_manifest:
        parser.error('SDK metadata 与 HyperOS vendor 共用 slot 2，开盘前拒绝同时输入')
    if args.sdk_ramdisk and args.sdk_partitions:
        parser.error('--sdk-ramdisk 与 --sdk-partitions 需要分别运行')
    sdk = bool(args.sdk_partitions or args.sdk_ramdisk)
    if args.hyperos_manifest:
        if any((args.runtime, args.desktop, args.root_metadata, args.system_image, sdk)):
            parser.error('HyperOS 固定清单与 SDK36 输入参数不能同时使用')
    elif not all((args.runtime, args.desktop, args.root_metadata)):
        parser.error('SDK36 需要 --runtime、--desktop 和 --root-metadata')
    if sdk and not args.system_image:
        parser.error('SDK36 分区与 ramdisk 入口需要 --system-image')
    if args.data_image and (not sdk or args.hyperos_manifest):
        parser.error('--data-image 需要 SDK36 分区或 ramdisk 入口')
    if args.metadata_image and not sdk:
        parser.error('--metadata-image 需要 SDK36 分区或 ramdisk 入口')
    if args.sdk_ramdisk and not args.lifecycle_only and not all((args.data_image, args.metadata_image)):
        parser.error('原 first-stage 运行需要独立的 --data-image 与 --metadata-image')
    source = args.report.resolve()
    base = json.loads(source.read_text())
    config = base['configuration']
    if (config['guest_layout'] != 'sparse' or config['workload'] != 'core' or config['cpus'] != 1 or
            config['execution'] != 'direct' or config['direct']['memory'] != 'flat' or not config['data_tbi'] or
            (config.get('guest_page_bytes', 16384) not in (4096, 16384) or
             (args.hyperos_manifest and config.get('guest_page_bytes', 16384) != 4096)) or config['mapping'] != 'pte' or config['code_memory'] != 'dual' or
            not base['runs'] or not all(r['validated'] for r in base['runs'])):
        parser.error('需要成功的 sparse 单 CPU Direct flat/TBI 核心构建，页大小须与原输入一致')
    layouts = [row for run in base['runs'] for row in run['results'] if row.get('config') == 'translation_guest_layout']
    if not layouts or any(row['guest_end'] != layouts[0]['guest_end'] for row in layouts):
        parser.error('核心报告缺少一致的 sparse 地址范围')
    address_limit = layouts[0]['guest_end']
    if address_limit not in (1 << 36, 1 << 39):
        parser.error('核心 sparse 地址范围不同')
    if args.hyperos_manifest and not args.lifecycle_only:
        required_aslr = {('linux_mmap_aslr_sysctl_bounds', ''), ('linux_mmap_aslr', 'minimum'), ('linux_mmap_aslr', 'maximum')}
        if address_limit != 1 << 39 or any(not required_aslr <= {(row.get('test'), row.get('case', '')) for row in run['results']
                                                                 if row.get('passed') is True} for run in base['runs']):
            parser.error('HyperOS 原 init 需要成功的 512 GiB sparse ASLR 核心检查')
    for name, expected in base['sha256'].items():
        if sha(ROOT / name) != expected:
            raise ValueError('基准输入变化：' + name)
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'work/native-linux-nce'):
        parser.error('输出需要位于独立的 work/native-linux-nce 子目录')
    out.mkdir(parents=True, exist_ok=False)
    try:
        if args.hyperos_manifest:
            from stock_init_hyperos import prepare as prepare_hyperos
            prepared = prepare_hyperos(args.hyperos_manifest.resolve(), out)
        else:
            prepared = prepare(args.runtime.resolve(), args.desktop.resolve(), args.root_metadata.resolve(), out,
                               args.system_image.resolve() if args.system_image else None, sdk_partitions=sdk)
        from exec_aslr import elf_spec
        program = elf_spec(Path(prepared['files']['/system/bin/init']['source']))
        interpreter = program['interpreter']
        prepared['exec_elf_images'] = {'/system/bin/init': program,
                                      interpreter: elf_spec(Path(prepared['files'][interpreter]['source']))}
        if args.sdk_ramdisk:
            from stock_init_first_stage import prepare as prepare_first_stage
            prepared = prepare_first_stage(args.sdk_ramdisk.resolve(strict=True), out, prepared)
            prepared['limitations'] = ['Raw original /init performs first-stage device discovery, mounts and root changes; progress is recorded separately from full boot.',
                                       'Mounted-partition preparation is used only by same-disk recovery.']
    except Exception as error:
        (out / 'failure.json').write_text(json.dumps({'stage': 'original_input_preparation', 'error': str(error)}, ensure_ascii=False, indent=2) + '\n')
        raise
    if not args.hyperos_manifest:
        clones = ROOT / ('work/cleanup-' + datetime.now().strftime('%Y-%m-%d'))
        clones.mkdir(exist_ok=True)
        clone_log = clones / (out.name + '-clones.json')
        clone_log.write_text(json.dumps({'method': 'cp -c', 'files': prepared['files']}, indent=2) + '\n')
        prepared['clone_manifest'] = {'path': str(clone_log), 'sha256': sha(clone_log)}
    prepared['namespace_stop_contract'] = 1
    if args.data_image:
        from stock_init_data import inspect as inspect_data, export_clean
        args.data_image = args.data_image.resolve(strict=True)
        if args.data_image.samefile(args.system_image):
            parser.error('数据盘与原系统盘需要独立文件')
        prepared['data_image'] = inspect_data(args.data_image)
        if prepared['data_image']['bytes'] != prepared['data_image']['filesystem_bytes']:
            parser.error('数据盘大小与 ext4 超级块不匹配')
    if args.metadata_image:
        from stock_init_metadata_volume import inspect as inspect_metadata_volume, ORIGINAL_SHA
        args.metadata_image = args.metadata_image.resolve(strict=True)
        metadata_original = args.system_image.resolve().with_name('encryptionkey.img')
        if sha(metadata_original) != ORIGINAL_SHA:
            parser.error('SDK 原 encryptionkey.img 摘要不匹配')
        if args.metadata_image.samefile(metadata_original) or args.metadata_image.samefile(args.system_image) or (args.data_image and args.metadata_image.samefile(args.data_image)):
            parser.error('metadata 可写盘需要与原盘、系统盘及数据盘独立')
        original_state = inspect_metadata_volume(metadata_original)
        prepared['metadata_image'] = inspect_metadata_volume(args.metadata_image)
        if any(prepared['metadata_image'][key] != original_state[key] for key in ('partition', 'gpt_prefix_sha256', 'gpt_suffix_sha256')):
            parser.error('metadata 候选的原 GPT 外围字节不同')
        prepared['metadata_original'] = {'path': str(metadata_original), 'sha256': ORIGINAL_SHA}
        partition = prepared['metadata_image']['partition']
        (out / 'metadata-partition.tsv').write_text('{name} {partition} {start_sector} {sectors} {whole_sectors}\n'.format(**partition))
    result = {key: base[key] for key in ('platform', 'cpu', 'system_build', 'dependency')}
    result.update(scope=('Original HyperOS 4 KiB init selinux_setup/second_stage observation; original partitions and full rc retained; first-stage and system boot unverified'
                         if args.hyperos_manifest else 'Original SDK36 init selinux_setup/second_stage observation; full rc retained; first-stage and system boot unverified'),
                  source_report=str(source), source_report_sha256=sha(source), source_configuration=config,
                  input=prepared, commands=[], runs=[], sha256=dict(base['sha256']))
    if args.lifecycle_only:
        result['scope'] = ('Original HyperOS 4 KiB partition metadata and PID 1 lifecycle observation; original init remains unexecuted'
                           if args.hyperos_manifest else 'Original SDK36 partition and PID 1 lifecycle observation; original init remains unexecuted')
    if args.sdk_ramdisk:
        result['scope'] = ('Original SDK36 ramdisk and PID 1 lifecycle observation' if args.lifecycle_only else
                           'Original SDK36 /init without stage arguments; first-stage progress, stop and same-disk recovery observed; full boot requires separate completion')
    for path in (Path(__file__).resolve(), EXP / 'translation/stock-init-guest.c', EXP / 'translation/stock-init-runner.h',
                 EXP / 'translation/stock-init-partitions.h', EXP / 'translation/stock-init-trace.h',
                 EXP / 'translation/stock-init-exec.h', ROOT / 'scripts/exec_aslr.py',
                 EXP / 'userspace/signal-tests.c', EXP / 'userspace/signal-registers.S',
                 ROOT / 'scripts/stock_init_metadata.py', ROOT / 'scripts/stock_init_sdk.py',
                 ROOT / 'scripts/prepare_android_runtime.py', out / 'files.tsv'):
        result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    for path in (EXP / 'translation/stock-init-dm-abi.h', EXP / 'translation/stock-init-dm.h'):
        result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    for path in (EXP / 'translation/stock-init-data.h', EXP / 'translation/stock-init-volume.h',
                 EXP / 'translation/stock-init-metadata.h', ROOT / 'scripts/stock_init_metadata_volume.py', ROOT / 'scripts/stock_init_data.py',
                 EXP / 'translation/stock-init-stop.h', ROOT / 'scripts/stock_init_stop.py'):
        result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    if args.metadata_image:
        result['sha256'][str((out / 'metadata-partition.tsv').relative_to(ROOT))] = sha(out / 'metadata-partition.tsv')
    if args.hyperos_manifest:
        for path in (ROOT / 'scripts/stock_init_hyperos.py', out / 'files.tsv', out / 'original-metadata.tsv'):
            result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    if sdk:
        for path in (out / 'original-partitions.tsv', out / 'original-metadata.tsv', out / 'original-system-dlkm-metadata.erofs',
                     ROOT / 'scripts/hyperos_erofs_metadata.py'):
            result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    if args.sdk_ramdisk:
        for path in (ROOT / 'scripts/stock_init_first_stage.py', ROOT / 'scripts/stock_init_sdk_ramdisk.py',
                     ROOT / 'scripts/stock_init_first_stage_devices.py', EXP / 'translation/stock-init-first-stage-devices.h',
                     EXP / 'translation/stock-init-ramdisk.h', EXP / 'translation/stock-init-first-stage-dm.h',
                     out / 'sdk-ramdisk.h', out / 'sdk-ramdisk.json'):
            result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    runner_source = (EXP / 'translation/linux-runner.c').read_text()
    start = runner_source.index('static void stock_bionic_setup(void)')
    end = runner_source.index('#include "stock-art-runner.h"', start)
    runner_source = runner_source[:start] + runner_source[end:]
    original = '    } else if (stock_bionic) {\n        stock_bionic_setup();\n        translation_memory_phase("guest_execution");\n        failed = run_program("/probe-guest", 1);'
    if runner_source.count(original) != 1:
        raise ValueError('运行器组件入口已变化')
    runner_source = runner_source.replace(original, '    } else if (stock_bionic) {\n        stock_init_setup();\n        translation_memory_phase("guest_execution");\n        failed = stock_init_run();')
    if sdk:
        release = '    REQUIRE(translation_kernel_release(&loaded) == 0);'
        if runner_source.count(release) != 1:
            raise ValueError('运行器内核释放位置已变化')
        runner_source = runner_source.replace(release, release + '\n    printf("{\\"stock_init_shutdown\\":true,\\"halt_result\\":0,\\"host_disks_closed\\":%s,\\"kernel_release_result\\":0}\\n", disk_close_result == 0 ? "true" : "false");')
    if args.sdk_ramdisk:
        runner_source = _first_stage_runner_source(runner_source)
    for shell_setup in ('    args[1] = (long)"/bin";\n    REQUIRE(call(34, args) == 0);\n',
                        '    put_file("/bin/sh", "dash.elf");\n'):
        if runner_source.count(shell_setup) != 1:
            raise ValueError('运行器 core shell 准备已变化')
        runner_source = runner_source.replace(shell_setup, '')
    disk_setup = ('    REQUIRE(!(getenv("NCE_STOCK_INIT_METADATA") && getenv("NCE_STOCK_INIT_METADATA")[0] && getenv("NCE_STOCK_INIT_VENDOR") && getenv("NCE_STOCK_INIT_VENDOR")[0]));\n'
                  '    REQUIRE(!getenv("NCE_STOCK_INIT_METADATA") || !getenv("NCE_STOCK_INIT_METADATA")[0] || (getenv("NCE_STOCK_INIT_LP") && !strcmp(getenv("NCE_STOCK_INIT_LP"), "1")));\n')
    disk_setup += ''.join('    if (getenv("%s") && getenv("%s")[0]) REQUIRE(nce_disk_open(%d, getenv("%s"), %d) == 0);\n' % (name, name, index, name, writable)
                          for index, name, writable in ((0, 'NCE_TRANSLATION_DATA', 1), (1, 'NCE_STOCK_INIT_SYSTEM', 0),
                                                        (2, 'NCE_STOCK_INIT_VENDOR', 0), (3, 'NCE_STOCK_INIT_SYSTEM_DLKM', 0),
                                                        (2, 'NCE_STOCK_INIT_METADATA', 1)))
    opening = ('    if (android) {\n'
               '        REQUIRE(getenv("NCE_TRANSLATION_SYSTEM"));\n'
               '        REQUIRE(nce_disk_open(1, getenv("NCE_TRANSLATION_SYSTEM"), 0) == 0);\n'
               '        if (services) {\n'
               '            REQUIRE(getenv("NCE_TRANSLATION_DATA"));\n'
               '            REQUIRE(nce_disk_open(0, getenv("NCE_TRANSLATION_DATA"), 1) == 0);\n'
               '        }\n'
               '    }\n')
    if runner_source.count(opening) != 1:
        raise ValueError('运行器原磁盘开盘位置已变化')
    runner_source = runner_source.replace(opening, disk_setup)
    runner_source = runner_source.replace('static int detach_preparation(void)', '#include "stock-init-runner.h"\n\nstatic int detach_preparation(void)')
    runner = out / 'linux-runner.c'
    runner.write_text(runner_source)
    result['sha256'][str(runner.relative_to(ROOT))] = sha(runner)
    execution = out / 'linux-execution.c'
    execution_text = (EXP / 'translation/linux-execution.c').read_text()
    execution_text = execution_text.replace('static void user_run(void *opaque)',
                                             '#include "stock-init-trace.h"\n#include "stock-init-exec.h"\n\n'
                                             '/* The init probe runs on one guest CPU and records both rows before user_return. */\n'
                                             'static unsigned long stock_init_exec_count;\n\nstatic void user_run(void *opaque)')
    before = '            long result = run_syscall(number, args);'
    if execution_text.count(before) != 1:
        raise ValueError('syscall 观察位置已变化')
    observed = ('            unsigned char exec_path[256] = {0};\n'
                '            if (number == 221) stock_init_trace_string(exec_path, args[0], sizeof(exec_path));\n'
                '            if (number == 142) printf("{\\"stock_init_reboot\\":true,\\"args\\":[%lu,%lu,%lu,%lu]}\\n", (unsigned long)args[0], (unsigned long)args[1], (unsigned long)args[2], (unsigned long)args[3]);\n'
                + ('            struct stock_init_trace_dm_sample dm_sample;\n'
                   '            stock_init_trace_dm_before(number, args, &dm_sample);\n' if args.sdk_ramdisk else '')
                + before + '\n'
                + ('            stock_init_trace_dm_after(&dm_sample, args, result);\n' if args.sdk_ramdisk else '') +
                '            unsigned long exec_index = 0;\n'
                '            if (number == 221 && result == 0 && (!strcmp((const char *)exec_path, "/system/bin/init")' +
                (' || !strcmp((const char *)exec_path, "/init")' if args.sdk_ramdisk else '') + '))\n'
                '                exec_index = ++stock_init_exec_count;\n'
                '            stock_init_trace_write(number, args, result);\n'
                '            stock_init_trace_security(number, args, result, exec_path, exec_index);\n'
                '            stock_init_trace_exec(exec_index, p);\n'
                '            stock_init_trace_block(number, args, result);\n'
                '            stock_init_trace_result(number, args, result, p->regs.pc);\n')
    execution_text = execution_text.replace(before, observed)
    cleanup_format = '\\"preemptions\\":%lu}\\n",'
    cleanup_args = 'allocated == freed ? "true" : "false", allocated, freed, maps, revocations, fills, faults, calls, preemptions);'
    if execution_text.count(cleanup_format) != 1 or execution_text.count(cleanup_args) != 1:
        raise ValueError('终末回收观察位置已变化')
    execution_text = execution_text.replace(cleanup_format, '\\"preemptions\\":%lu,\\"stock_init_exec_count\\":%lu}\\n",')
    execution_text = execution_text.replace(cleanup_args, cleanup_args[:-2] + ', stock_init_exec_count);')
    execution.write_text(execution_text)
    result['sha256'][str(execution.relative_to(ROOT))] = sha(execution)
    for path in source.parent.iterdir():
        if path.suffix in ('.elf', '.so') and path.name not in ('guest.elf', 'late.elf'):
            (out / path.name).symlink_to(path)
    compile_guest = next(list(cmd) for cmd in base['commands'] if '-c' in cmd and str(EXP / 'translation/linux-guest.c') in cmd)
    compile_guest[compile_guest.index(str(EXP / 'translation/linux-guest.c'))] = str(EXP / 'translation/stock-init-guest.c')
    compile_guest[compile_guest.index('-o') + 1] = str(out / 'guest.o')
    compile_guest += ['-D_GNU_SOURCE']
    link_guest = next(list(cmd) for cmd in base['commands'] if '-o' in cmd and cmd[cmd.index('-o') + 1] == str(source.parent / 'guest.elf'))
    link_guest = [value for value in link_guest if not (str(source.parent) in value and value.endswith('.o'))]
    link_guest.insert(link_guest.index('-o'), str(out / 'guest.o'))
    link_guest[link_guest.index('-o') + 1] = str(out / 'guest.elf')
    signal_commands = []
    for name in ('signal-tests.c', 'signal-registers.S'):
        signal_source = str(EXP / 'userspace' / name)
        command = next(list(cmd) for cmd in base['commands'] if '-c' in cmd and signal_source in cmd)
        output = out / (Path(name).stem + '.o')
        command[command.index('-o') + 1] = str(output)
        signal_commands.append(command)
        link_guest.insert(link_guest.index('-o'), str(output))
    for command in (compile_guest, *signal_commands, link_guest):
        subprocess.run(command, check=True, timeout=120)
        result['commands'].append(command)
    (out / 'late.elf').symlink_to(out / 'guest.elf')
    result['sha256'][str((out / 'guest.elf').relative_to(ROOT))] = sha(out / 'guest.elf')
    keys = {'GUEST_LAYOUT': 'guest_layout', 'DATA_TBI': 'data_tbi', 'ONLINE_CPUS': 'online_cpus',
            'BATCH': 'instruction_batch', 'PREEMPTION': 'preemption', 'MEMORY_MS': 'memory_interval_ms',
            'CACHE': 'cache_invalidation', 'EXEC_CACHE': 'exec_cache', 'TLB': 'tlb_invalidation'}
    selected = {'NCE_TRANSLATION_' + key: str(int(config[value]) if type(config[value]) is bool else config[value]) for key, value in keys.items()}
    selected.update({'NCE_DIRECT_' + key.upper(): str(int(value) if type(value) is bool else value) for key, value in config['direct'].items()})
    selected.update(NCE_TRANSLATION_FILES=str(out), NCE_TRANSLATION_WORKLOAD='bionic',
                    NCE_TRANSLATION_CACHE_MIB=str(config['translation_cache_bytes'] // (1 << 20)), NCE_STOCK_INIT_DIRECTORY=str(out))
    selected['NCE_STOCK_INIT_SYSTEM'] = (prepared.get('deployed_system_partition') or {}).get('path', '')
    if sdk:
        selected['NCE_STOCK_INIT_LP'] = '1'
    if args.sdk_ramdisk:
        selected['NCE_STOCK_INIT_FIRST_STAGE'] = '1'
    if args.data_image:
        selected['NCE_TRANSLATION_DATA'] = str(args.data_image)
    if args.metadata_image:
        selected['NCE_STOCK_INIT_METADATA'] = str(args.metadata_image)
    if args.hyperos_manifest:
        selected.update(NCE_STOCK_INIT_VENDOR=prepared['original_vendor']['path'],
                        NCE_STOCK_INIT_SYSTEM_DLKM=prepared['original_system_dlkm']['path'])
    result['test_environment'] = selected
    for entry in ('cli', 'gcd'):
        command = next(list(cmd) for cmd in base['commands'] if '-o' in cmd and Path(cmd[cmd.index('-o') + 1]).name == entry)
        includes = [command[i + 1] for i, value in enumerate(command[:-1]) if value == '-I']
        kernel = next(Path(path) / 'lkl-image.bin' for path in includes if (Path(path) / 'lkl-image.bin').is_file())
        command[command.index(str(EXP / 'translation/linux-runner.c'))] = str(runner)
        command[command.index(str(EXP / 'translation/linux-execution.c'))] = str(execution)
        command[command.index('-o') + 1] = str(out / entry)
        command += ['-iquote', str(EXP / 'translation')]
        if args.sdk_ramdisk:
            command += ['-DNCE_STOCK_INIT_RAMDISK', '-iquote', str(out)]
        subprocess.run(command, check=True, timeout=120)
        result['commands'].append(command)
        result['sha256'][str((out / entry).relative_to(ROOT))] = sha(out / entry)
    stages = [('lifecycle', 'lifecycle')]
    if not args.lifecycle_only:
        stages.append(('original', 'original'))
        if args.data_image or args.metadata_image:
            stages.append(('lifecycle', 'recovery'))
    volumes = []
    if args.data_image:
        volumes.append(('data', args.data_image, inspect_data))
    if args.metadata_image:
        volumes.append(('metadata', args.metadata_image, inspect_metadata_volume))
    previous_generation = {name: None for name, _, _ in volumes}
    for mode, phase in stages:
        for entry in ('cli', 'gcd'):
            env = {**{key: value for key, value in os.environ.items() if not key.startswith('NCE_')}, **selected, 'NCE_STOCK_INIT_MODE': mode}
            first_stage = bool(args.sdk_ramdisk and phase != 'recovery')
            if args.sdk_ramdisk:
                env['NCE_STOCK_INIT_FIRST_STAGE'] = '1' if first_stage else '0'
            label = entry + '-' + phase
            states = {}
            for name, path, inspector in volumes:
                try:
                    before = inspector(path)
                    states[name + '_before'] = before
                    expected = result['runs'][-1][name + '_after'] if result['runs'] else prepared[name + '_image']
                    if before['sha256'] != expected['sha256']:
                        raise ValueError('两次运行之间的磁盘摘要变化：' + name)
                except (ValueError, OSError) as error:
                    failure = {'stage': name + '_before_run', 'entry': entry, 'mode': mode, 'phase': phase, 'error': str(error)}
                    result[name + '_state_error'] = failure
                    (out / 'failure.json').write_text(json.dumps(failure, ensure_ascii=False, indent=2) + '\n')
                    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
                    raise
            with (out / (label + '.jsonl')).open('w') as stdout, (out / (label + '.log')).open('w') as stderr:
                try:
                    proc = subprocess.run([out / entry, kernel], env=env, stdout=stdout, stderr=stderr, timeout=90)
                    returncode = proc.returncode
                except subprocess.TimeoutExpired:
                    returncode = 'timeout'
            stderr = (out / (label + '.log')).read_text()
            output = out / (label + '.jsonl')
            try:
                rows = read_observations(output)
            except ValueError as error:
                for name, path, inspector in volumes:
                    try:
                        states[name + '_after'] = inspector(path)
                    except (ValueError, OSError) as inspect_error:
                        states[name + '_after'] = None
                        states[name + '_after_error'] = str(inspect_error)
                failure = {'stage': 'observation_json_parse', 'entry': entry, 'mode': mode,
                           'phase': phase,
                           'returncode': returncode, 'error': str(error),
                           'raw_stdout': str(output), 'raw_stdout_sha256': sha(output),
                           **states}
                result['runs'].append({**failure, 'observation_validated': False, 'results': [], 'stderr': stderr})
                (out / 'failure.json').write_text(json.dumps(failure, ensure_ascii=False, indent=2) + '\n')
                (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
                raise SystemExit(str(error))
            observation_errors = []
            try:
                valid = returncode == 0 and validate(rows, mode, bool(selected['NCE_STOCK_INIT_SYSTEM']),
                                                   guest_page_bytes=config.get('guest_page_bytes', 16384), kernel_log=stderr,
                                                   original_input=prepared if args.hyperos_manifest or sdk else None,
                                                   exec_elf_images=prepared['exec_elf_images'], first_stage=first_stage) and 'stock_init_exec_errno' not in stderr
            except (ValueError, TypeError, KeyError, IndexError) as error:
                valid = False
                observation_errors.append({'stage': 'observation_validation', 'type': type(error).__name__, 'error': str(error)})
            for name, path, inspector in volumes:
                try:
                    after = inspector(path)
                    states[name + '_after'] = after
                except (ValueError, OSError) as error:
                    states[name + '_after'] = None
                    states[name + '_after_error'] = str(error)
                    valid = False
                    continue
                keys = ('uuid', 'bytes', 'filesystem_bytes', 'block_bytes')
                if name == 'metadata':
                    keys += ('partition', 'gpt_prefix_sha256', 'gpt_suffix_sha256')
                identity = states[name + '_before'] if args.sdk_ramdisk else prepared[name + '_image']
                if any(after[key] != identity[key] for key in keys if not first_stage or key != 'uuid'):
                    valid = False
                if valid and not first_stage:
                    generation = next(row for row in rows if 'stock_init_' + name + '_generation' in row)
                    if previous_generation[name] is not None and generation['previous'] != previous_generation[name]:
                        valid = False
                    else:
                        previous_generation[name] = generation['generation']
                if mode == 'lifecycle' and not after['clean']:
                    valid = False
                if first_stage and mode == 'lifecycle' and after['sha256'] != states[name + '_before']['sha256']:
                    valid = False
            metadata_rc = {}
            guest_writes = ''
            progress = {}
            sysfs_devices = {}
            try:
                observation_stage = 'metadata_rc'
                if args.metadata_image:
                    from stock_init_metadata_volume import rc_results
                    metadata_rc = {'metadata_rc': rc_results(rows)}
                observation_stage = 'guest_write_text'
                guest_writes = ''.join(bytes.fromhex(row['hex']).decode('utf-8', errors='replace') for row in rows
                                       if 'stock_init_write' in row and bytes.fromhex(row['hex']).startswith(b'<'))
                observation_stage = 'first_stage_sysfs'
                if args.sdk_ramdisk:
                    from stock_init_first_stage_devices import observe as observe_devices
                    sysfs_devices = {'sysfs_devices': observe_devices(rows, prepared, first_stage)}
                observation_stage = 'first_stage_progress'
                if first_stage and mode == 'original':
                    from stock_init_first_stage import progress as first_stage_progress
                    progress = {'first_stage': first_stage_progress(rows, prepared)}
            except (ValueError, TypeError, KeyError, IndexError) as error:
                valid = False
                observation_errors.append({'stage': observation_stage, 'type': type(error).__name__, 'error': str(error)})
            result['runs'].append({'entry': entry, 'mode': mode, 'phase': phase, 'returncode': returncode, 'observation_validated': valid,
                                   'profile': 'sdk36-first-stage' if first_stage else 'hyperos' if args.hyperos_manifest else 'sdk36-lp' if sdk else 'sdk36',
                                   **progress, **sysfs_devices,
                                   **states, **metadata_rc,
                                   'raw_stdout': str(output), 'raw_stdout_sha256': sha(output), 'observation_errors': observation_errors,
                                   'results': rows, 'stderr': stderr, 'guest_write_text': guest_writes,
                                   'attempted_log_messages': {text: text in stderr or text in guest_writes for text in ('Loading SELinux policy', 'init second stage started!', 'SetupMountNamespaces done')}})
            (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            print(label, 'observed=' + str(valid), flush=True)
            if not valid:
                (out / 'failure.json').write_text(json.dumps({'stage': 'init_observation_validation', 'entry': entry,
                                                            'mode': mode, 'phase': phase, 'returncode': returncode,
                                                            'raw_stdout': str(output), 'raw_stdout_sha256': sha(output),
                                                            'observation_errors': observation_errors,
                                                            **states}, indent=2) + '\n')
                raise SystemExit('init 观察或回收检查失败：' + str(out / 'summary.json'))
    if args.data_image:
        try:
            result['data_export'] = export_clean(args.data_image, out / 'data-export')
            if result['data_export']['generation'] != previous_generation['data']:
                raise ValueError('恢复后的磁盘 generation 不匹配')
            result['data_persistence_validated'] = True
            result['original_sqlite_validated'] = (result['data_export'].get('quick_check') == ['ok'] and
                                                   result['data_export'].get('sqlite_header') == b'SQLite format 3\0'.hex() and
                                                   'sqlite_error' not in result['data_export'])
            if not args.lifecycle_only and not args.sdk_ramdisk and not result['original_sqlite_validated']:
                raise ValueError('原 Keystore SQLite 文件格式或 quick_check 检查失败')
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            result['data_export_error'] = str(error)
            (out / 'failure.json').write_text(json.dumps({'stage': 'data_export', 'error': str(error)}, ensure_ascii=False, indent=2) + '\n')
            (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            raise
        (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    if args.metadata_image:
        original_runs = [run for run in result['runs'] if run['mode'] == 'original']
        result['metadata_persistence_validated'] = (all(run['observation_validated'] for run in result['runs']) and
                                                    result['runs'][-1]['metadata_after']['clean'] and
                                                    (args.lifecycle_only or len([run for run in result['runs'] if run['phase'] == 'recovery']) == 2))
        recovery_runs = [run for run in result['runs'] if run['phase'] == 'recovery']
        result['metadata_rc_validated'] = (bool(original_runs) and len(recovery_runs) == 2 and
                                           all(run['metadata_rc']['apex_sessions_opened'] for run in original_runs) and
                                           all(run['metadata_rc']['directories_match_original_rc'] is True for run in recovery_runs))
        result['metadata_directory_validation_source'] = 'same_disk_recovery' if result['metadata_rc_validated'] else None
        result['metadata_final_generation'] = previous_generation['metadata']
        result['metadata_original_unchanged'] = sha(metadata_original) == ORIGINAL_SHA
        (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
        from stock_init_metadata_volume import require_complete
        require_complete(result, args.lifecycle_only or bool(args.sdk_ramdisk))
    if args.sdk_ramdisk:
        original_runs = [run for run in result['runs'] if run['mode'] == 'original']
        result['first_stage_completed'] = bool(original_runs) and all(run['first_stage']['completed'] for run in original_runs)
        result['full_boot_observed'] = None
        result['full_boot_validation'] = 'pending'
    if any(sha(ROOT / name) != expected for name, expected in result['sha256'].items()):
        raise ValueError('运行期间构建输入变化')
    if any(sha(Path(item['source'])) != item['sha256'] for item in prepared['files'].values()):
        raise ValueError('运行期间原始输入变化')
    if args.system_image and sha(args.system_image) != SYSTEM_IMAGE_SHA:
        raise ValueError('运行期间原始系统镜像变化')
    if args.sdk_ramdisk and sha(args.sdk_ramdisk) != prepared['sdk_ramdisk']['input_sha256']:
        raise ValueError('运行期间原始 ramdisk 变化')
    if args.hyperos_manifest:
        for name in ('deployed_system_partition', 'original_vendor', 'original_system_dlkm'):
            record = prepared[name]
            if sha(Path(record['path'])) != record['sha256']:
                raise ValueError('运行期间原 HyperOS 分区变化：' + name)
        if sha(args.hyperos_manifest) != prepared['hyperos_manifest_sha256']:
            raise ValueError('运行期间原 HyperOS 固定清单变化')
    (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    if args.sdk_ramdisk:
        from stock_init_first_stage import require_complete as require_first_stage_complete
        try:
            require_first_stage_complete(result, args.lifecycle_only)
        except ValueError as error:
            result['first_stage_completion_error'] = str(error)
            (out / 'failure.json').write_text(json.dumps({'stage': 'first_stage_completion', 'error': str(error)}, ensure_ascii=False, indent=2) + '\n')
            (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            raise SystemExit(str(error) + '：' + str(out / 'summary.json'))
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
