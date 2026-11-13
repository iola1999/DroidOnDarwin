"""准备原 SDK36 ramdisk 输入，并检查 first-stage 的独立观察结果。"""
import json
import re
import stat

from exec_aslr import elf_spec_bytes, validate_exec_layout
from stock_init_first_stage_devices import observe as observe_devices, validate as validate_devices
from stock_init_sdk_ramdisk import read

PROFILE = 'sdk36-first-stage'
CMDLINE = 'androidboot.hardware=ranchu'
INIT_PATHS = ('/init', '/system/bin/init')


def prepare(ramdisk, output, mounted_input):
    parsed = read(ramdisk)
    entries = parsed['entries']
    program = next(row for row in entries if row['path'] == 'init')
    spec = elf_spec_bytes(program['data'], '/init')
    if (spec['type'] != 2 or spec['interpreter'] is not None or
            any(row['nlink'] != 1 for row in entries)):
        raise ValueError('固定原 ramdisk 的 ELF 或 nlink 合同不同')
    payload = bytearray()
    offsets = []
    for row in entries:
        offsets.append(len(payload))
        payload.extend(row['data'])
    header = output / 'sdk-ramdisk.h'
    with header.open('w') as stream:
        stream.write('/* 本轮固定 ramdisk 的原文件字节与部署属性。 */\n#pragma once\n')
        stream.write('struct stock_init_ramdisk_entry {\n'
                     '    const char *path;\n'
                     '    const unsigned char *data;\n'
                     '    unsigned long bytes;\n'
                     '    unsigned archive, mode, uid, gid, mtime, rdev_major, rdev_minor, nlink;\n'
                     '};\nstatic const unsigned char stock_init_ramdisk_data[] = {\n')
        for offset in range(0, len(payload), 24):
            stream.write('    ' + ','.join(f'0x{byte:02x}' for byte in payload[offset:offset + 24]) + ',\n')
        stream.write('};\nstatic const struct stock_init_ramdisk_entry stock_init_ramdisk_entries[] = {\n')
        for row, offset in zip(entries, offsets):
            stream.write('    {%s, stock_init_ramdisk_data + %d, %d, %d, %d, %d, %d, %d, %d, %d, %d},\n' %
                         (json.dumps('/' + row['path']), offset, row['bytes'], row['archive'], row['mode'],
                          row['uid'], row['gid'], row['mtime'], row['rdev_major'], row['rdev_minor'], row['nlink']))
        stream.write('};\n')
    manifest = {key: value for key, value in parsed.items() if key != 'entries'}
    manifest.update(profile=PROFILE, cmdline=CMDLINE,
                    entries=[{key: value for key, value in row.items() if key != 'data'} for row in entries],
                    header=str(header), payload_bytes=len(payload),
                    deployment='original regular-file bytes embedded in this runner; inode identifiers assigned by Linux')
    manifest_path = output / 'sdk-ramdisk.json'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    return {**mounted_input, 'sdk_ramdisk': manifest, 'first_stage_contract': 1, 'host_dm_cleanup_contract': 1,
            'exec_elf_images': {**mounted_input['exec_elf_images'], '/init': spec}}


def _uint(value, bits=64):
    return type(value) is int and 0 <= value < 1 << bits


def _hex(value, limit):
    if type(value) is not str or len(value) > limit or len(value) % 2:
        return False
    return all(character in '0123456789abcdef' for character in value)


def _ordered(rows, events):
    positions = [rows.index(event) for event in events]
    return all(before < after for before, after in zip(positions, positions[1:]))


def _validate_dm_cleanup(rows, *, host_cleanup=False):
    controls = [row for row in rows if 'stock_init_first_stage_dm_control' in row]
    ioctls = [row for row in rows if 'stock_init_first_stage_dm_ioctl' in row]
    prepares = [row for row in rows if 'stock_init_first_stage_dm_prepare' in row]
    cleanups = [row for row in rows if 'stock_init_first_stage_dm_cleanup' in row]
    operations = [('prepare', operation) for operation in ('mknod', 'open', 'duplicate', 'close_initial', 'unlink')]
    operations.append(('cleanup', 'close_control'))
    if (len(controls) != 6 or len(ioctls) != 3 or len(prepares) != 1 or len(cleanups) != 1 or
            [(row.get('phase'), row.get('operation')) for row in controls] != operations):
        return False
    for row in controls:
        if (set(row) != {'stock_init_first_stage_dm_control', 'phase', 'operation', 'fd', 'result'} or
                row['stock_init_first_stage_dm_control'] is not True or
                type(row['fd']) is not int or type(row['result']) is not int):
            return False
    opened, control = controls[1]['result'], controls[2]['result']
    if (not _uint(opened, 31) or not 3 <= control < 1 << 31 or control == opened or
            controls != [{'stock_init_first_stage_dm_control': True, 'phase': phase, 'operation': operation,
                          'fd': fd, 'result': result}
                         for (phase, operation), fd, result in zip(operations,
                                                                   (-1, opened, opened, opened, -1, control),
                                                                   (0, opened, control, 0, 0, 0))]):
        return False
    expected = [('prepare', 0), ('cleanup', 1), ('cleanup', 2)]
    fields = {'stock_init_first_stage_dm_ioctl', 'phase', 'command', 'result', 'version', 'flags',
              'data_size', 'data_start', 'dev', 'first_dev_observed', 'first_dev_within_data_size', 'first_dev'}
    if [(row.get('phase'), row.get('command')) for row in ioctls] != expected:
        return False
    denied = host_cleanup and all(row.get('result') == -13 for row in ioctls[1:])
    for row in ioctls:
        listed = row['command'] == 2
        denied_request = denied and row['phase'] == 'cleanup'
        if (set(row) != fields or row['stock_init_first_stage_dm_ioctl'] is not True or
                type(row['result']) is not int or row['result'] != (-13 if denied_request else 0) or
                not all(_uint(row[key], 32) for key in ('command', 'flags', 'data_size', 'data_start')) or
                not all(_uint(row[key]) for key in ('dev', 'first_dev')) or
                type(row['version']) is not list or len(row['version']) != 3 or
                not all(_uint(value, 32) for value in row['version']) or
                row['version'][0] != 4 or row['version'][1] < 48 or
                (denied_request and row['version'] != [4, 48, 0]) or
                row['flags'] != 0 or row['data_start'] != 312 or
                row['data_size'] != (312 if listed and not denied_request else 1024) or
                row['dev'] != 0 or row['first_dev'] != 0 or
                row['first_dev_observed'] is not listed or row['first_dev_within_data_size'] is not False):
            return False
    if (prepares != [{'stock_init_first_stage_dm_prepare': True, 'result': 0, 'control_fd': control}] or
            cleanups != [{'stock_init_first_stage_dm_cleanup': True, 'result': 0, 'control_fd': control, 'empty': True}] or
            prepares[0]['stock_init_first_stage_dm_prepare'] is not True or
            cleanups[0]['stock_init_first_stage_dm_cleanup'] is not True or cleanups[0]['empty'] is not True or
            any(type(row[key]) is not int for row in (*prepares, *cleanups) for key in ('result', 'control_fd'))):
        return False
    reaped = [row for row in rows if 'stock_init_host_stop_reaped' in row]
    synced = [row for row in rows if 'stock_init_first_stage_sync' in row]
    if (len(reaped) != 1 or synced != [{'stock_init_first_stage_sync': True, 'result': 0}] or
            synced[0]['stock_init_first_stage_sync'] is not True or type(synced[0]['result']) is not int):
        return False
    host = []
    if host_cleanup:
        host = [row for row in rows if 'stock_init_first_stage_dm_host_cleanup' in row]
        if (host != [{'stock_init_first_stage_dm_host_cleanup': True, 'syscall': 248, 'result': 0, 'empty': True}] or
                host[0]['stock_init_first_stage_dm_host_cleanup'] is not True or host[0]['empty'] is not True or
                any(type(host[0][key]) is not int for key in ('syscall', 'result'))):
            return False
    return _ordered(rows, (*controls[:5], ioctls[0], prepares[0], reaped[0], synced[0],
                           ioctls[1], ioctls[2], *host, controls[-1], cleanups[0]))


def _validate_dm_host_boundaries(rows, mode, prepared, cmdline, ready, observation):
    boundaries = [row for row in rows if 'stock_init_first_stage_dm_host_boundary' in row]
    if len(boundaries) != 2:
        return False
    parameters, running = boundaries
    if (set(parameters) != {'stock_init_first_stage_dm_host_boundary', 'operation', 'results'} or
            parameters['stock_init_first_stage_dm_host_boundary'] is not True or
            parameters['operation'] != 'parameters' or type(parameters['results']) is not list or
            parameters['results'] != [-22] * 6 or any(type(value) is not int for value in parameters['results']) or
            set(running) != {'stock_init_first_stage_dm_host_boundary', 'operation', 'result', 'ancestor_pid'} or
            running['stock_init_first_stage_dm_host_boundary'] is not True or running['operation'] != 'running_namespace' or
            type(running['result']) is not int or running['result'] != -16 or
            not _uint(running['ancestor_pid'], 31) or running['ancestor_pid'] == 0 or
            type(observation['ancestor_pid']) is not int or running['ancestor_pid'] != observation['ancestor_pid'] or
            not _ordered(rows, (prepared, parameters, cmdline, ready, running, observation))):
        return False
    if mode == 'original':
        executed = [row for row in rows if 'stock_init_exec' in row]
        if len(executed) != 1 or not _uint(executed[0].get('pid'), 31) or executed[0]['pid'] == 0:
            return False
        roles = {'namespace': executed[0]['pid']}
    elif mode == 'lifecycle':
        lifecycle = [row for row in rows if 'stock_init_lifecycle' in row]
        ordinary = [row for row in rows if 'stock_init_owned_ordinary' in row]
        ordinary_ready = [row for row in rows if 'stock_init_ordinary_ready' in row]
        if (len(lifecycle) != 1 or len(ordinary) != 1 or len(ordinary_ready) != 1 or
                any(not _uint(lifecycle[0].get(key), 31) or lifecycle[0][key] == 0 for key in ('pid', 'child')) or
                not _uint(ordinary[0].get('pid'), 31) or ordinary[0]['pid'] == 0 or
                type(ordinary[0].get('ready_pid')) is not int or
                type(ordinary_ready[0].get('pid')) is not int or
                ordinary[0]['ready_pid'] != ordinary[0]['pid'] or ordinary_ready[0]['pid'] != ordinary[0]['pid']):
            return False
        roles = {'namespace': lifecycle[0]['pid'], 'child': lifecycle[0]['child'], 'ordinary': ordinary[0]['pid']}
    else:
        return False
    denied = [row for row in rows if 'stock_init_first_stage_dm_guest_denied' in row]
    if len(denied) != len(roles):
        return False
    cleanup = next(row for row in rows if 'stock_init_first_stage_dm_cleanup' in row)
    stdout = next(row for row in rows if 'stock_init_output' in row and row['fd'] == 1)
    for row in denied:
        if (set(row) != {'stock_init_first_stage_dm_guest_denied', 'pid', 'role', 'errno'} or
                row['stock_init_first_stage_dm_guest_denied'] is not True or not _uint(row['pid'], 31) or row['pid'] == 0 or
                type(row['role']) is not str or row['role'] not in roles or row['pid'] != roles[row['role']] or
                type(row['errno']) is not int or row['errno'] != 38 or
                not _ordered(rows, (cleanup, row, stdout))):
            return False
    return len({row['role'] for row in denied}) == len(roles)


def _validate_table_rows(rows):
    fields = {'stock_init_dm_table_load', 'pid', 'fd', 'result', 'read_result', 'version',
              'data_size', 'data_start', 'target_count', 'flags', 'dev', 'name_hex', 'targets'}
    for row in (row for row in rows if 'stock_init_dm_table_load' in row):
        if (set(row) != fields or row['stock_init_dm_table_load'] is not True or
                not _uint(row['pid'], 31) or row['pid'] == 0 or not _uint(row['fd'], 31) or
                any(type(row[key]) is not int or not -4095 <= row[key] < 1 << 63 for key in ('result', 'read_result')) or
                type(row['version']) is not list or len(row['version']) != 3 or
                not all(_uint(value, 32) for value in row['version']) or
                not all(_uint(row[key], 32) for key in ('data_size', 'data_start', 'target_count', 'flags')) or
                not _uint(row['dev']) or not _hex(row['name_hex'], 256) or
                row['read_result'] not in (0, -7, -14, -22) or
                type(row['targets']) is not list or len(row['targets']) > 1):
            return False
        for target in row['targets']:
            base = {'sector_start', 'length', 'type_hex'}
            numeric = {'backing_major', 'backing_minor', 'backing_start_sector'}
            path = {'backing_path_hex', 'backing_start_sector'}
            if (type(target) is not dict or set(target) not in (base, base | numeric, base | path) or
                    not _uint(target['sector_start']) or not _uint(target['length']) or
                    not _hex(target['type_hex'], 32)):
                return False
            if row['read_result'] == 0 and (
                    len(target['type_hex']) > 30 or
                    (target['type_hex'] == b'linear'.hex()) != (set(target) != base)):
                return False
            if row['read_result'] != 0 and set(target) != base:
                return False
            if set(target) != base:
                if (target['type_hex'] != b'linear'.hex() or not _uint(target['backing_start_sector']) or
                        (set(target) == base | numeric and
                         (not _uint(target['backing_major'], 12) or not _uint(target['backing_minor'], 20))) or
                        (set(target) == base | path and not _hex(target['backing_path_hex'], 510))):
                    return False
                if set(target) == base | path:
                    value = bytes.fromhex(target['backing_path_hex'])
                    if (not value.startswith(b'/dev/block/') or not 11 < len(value) < 256 or
                            b'\0' in value or b' ' in value or
                            any(part in (b'', b'.', b'..') for part in value[1:].split(b'/'))):
                        return False
        if row['read_result'] == 0 and (
                row['target_count'] != 1 or len(row['targets']) != 1 or row['version'][0] != 4 or
                len(row['name_hex']) > 254 or row['data_start'] < 312 or row['data_start'] % 8 or
                row['data_size'] - row['data_start'] <= 40):
            return False
    return True


def validate(rows, mode, inputs, guest, page):
    if type(inputs) is not dict:
        return False
    host_cleanup = 'host_dm_cleanup_contract' in inputs
    if host_cleanup and (type(inputs['host_dm_cleanup_contract']) is not int or inputs['host_dm_cleanup_contract'] != 1):
        return False
    if not host_cleanup and any(any(key in row for key in ('stock_init_first_stage_dm_host_cleanup',
                                                          'stock_init_first_stage_dm_host_boundary',
                                                          'stock_init_first_stage_dm_guest_denied')) for row in rows):
        return False
    if (type(inputs.get('first_stage_contract')) is not int or inputs['first_stage_contract'] != 1 or
            not _validate_dm_cleanup(rows, host_cleanup=host_cleanup) or not _validate_table_rows(rows) or
            not validate_devices(rows, inputs, True)):
        return False
    manifest = inputs.get('sdk_ramdisk', {})
    entries = manifest.get('entries', [])
    actual = [row for row in rows if 'stock_init_ramdisk_entry' in row]
    if len(entries) != 29 or len(actual) != len(entries):
        return False
    fields = {'stock_init_ramdisk_entry', 'archive', 'path', 'mode', 'uid', 'gid', 'nlink', 'size',
              'mtime_sec', 'mtime_nsec', 'rdev_major', 'rdev_minor', 'content_matches'}
    for index, (expected, row) in enumerate(zip(entries, actual), 1):
        integers = fields - {'path', 'content_matches'}
        if (set(row) != fields or any(not _uint(row[key]) for key in integers) or
                row['stock_init_ramdisk_entry'] != index or row['archive'] != expected['archive'] or
                row['path'] != '/' + expected['path'] or row['mode'] != expected['mode'] or
                row['uid'] != expected['uid'] or row['gid'] != expected['gid'] or
                row['mtime_sec'] != expected['mtime'] or row['mtime_nsec'] != 0 or
                row['rdev_major'] != expected['rdev_major'] or row['rdev_minor'] != expected['rdev_minor'] or
                row['content_matches'] is not True or row['nlink'] == 0 or
                (stat.S_IFMT(expected['mode']) != stat.S_IFDIR and row['nlink'] != expected['nlink']) or
                (stat.S_IFMT(expected['mode']) == stat.S_IFREG and row['size'] != expected['bytes'])):
            return False
    cmdline = [row for row in rows if 'stock_init_first_stage_cmdline' in row]
    ready = [row for row in rows if 'stock_init_ramdisk_ready' in row]
    if (len(cmdline) != 1 or set(cmdline[0]) != {'stock_init_first_stage_cmdline', 'value_hex'} or
            cmdline[0]['stock_init_first_stage_cmdline'] is not True or not _hex(cmdline[0]['value_hex'], 8190) or
            ready != [{'stock_init_ramdisk_ready': True, 'entries': 29, 'proc_restored': True}] or
            ready[0]['stock_init_ramdisk_ready'] is not True or ready[0]['proc_restored'] is not True or
            type(ready[0]['entries']) is not int or not _ordered(rows, (cmdline[0], *actual, ready[0]))):
        return False
    text = bytes.fromhex(cmdline[0]['value_hex']).decode('ascii', errors='replace').strip('\n')
    hardware = [word for word in text.split(' ') if word.startswith('androidboot.hardware=')]
    if hardware != [CMDLINE] or '\0' in text:
        return False
    prepared = next(row for row in rows if 'stock_init_first_stage_dm_prepare' in row)
    observation = next(row for row in rows if 'stock_init_observation' in row)
    sysfs_mounts = [row for row in rows if 'stock_init_first_stage_sysfs_mount' in row]
    if not _ordered(rows, (prepared, cmdline[0], actual[-1], *sysfs_mounts, ready[0], observation)):
        return False
    first_exec = [row for row in rows if row.get('stock_init_security') == 221 and row.get('result') == 0 and
                  row.get('path_hex') == '/init'.encode().hex()]
    if first_exec and not _ordered(rows, (ready[0], first_exec[0], observation)):
        return False
    output = [row for row in rows if 'stock_init_output' in row]
    if (len(output) != 2 or [row.get('fd') for row in output] != [1, 2] or
            any(set(row) != {'stock_init_output', 'fd', 'bytes', 'read_result'} or
                row['stock_init_output'] is not True or not _uint(row['fd'], 31) or not _uint(row['bytes']) or
                type(row['read_result']) is not int or row['read_result'] != 0 for row in output)):
        return False
    cleanup = next(row for row in rows if 'stock_init_first_stage_dm_cleanup' in row)
    if host_cleanup and not _validate_dm_host_boundaries(rows, mode, prepared, cmdline[0], ready[0], observation):
        return False
    kernel_log = next(row for row in rows if 'stock_init_kernel_log' in row)
    if kernel_log['read_result'] < 0 and (mode != 'original' or kernel_log['read_result'] not in (-1, -13)):
        return False
    disks = [row for row in rows if 'host_disk_close' in row]
    slots = ([0] if inputs.get('data_image') else []) + [1] + ([2] if inputs.get('metadata_image') else [])
    if (disks != [{'host_disk_close': True, 'slot': slot, 'sync_attempted': slot in (0, 2), 'sync_result': 0, 'close_result': 0}
                  for slot in slots] or
            any(row['host_disk_close'] is not True or type(row['sync_attempted']) is not bool or
                any(type(row[key]) is not int for key in ('slot', 'sync_result', 'close_result')) for row in disks) or
            not _ordered(rows, (cleanup, *output, kernel_log, *disks))):
        return False
    shutdown = [row for row in rows if 'stock_init_shutdown' in row]
    if (shutdown != [{'stock_init_shutdown': True, 'halt_result': 0, 'host_disks_closed': True, 'kernel_release_result': 0}] or
            shutdown[0]['stock_init_shutdown'] is not True or shutdown[0]['host_disks_closed'] is not True or
            type(shutdown[0]['halt_result']) is not int or type(shutdown[0]['kernel_release_result']) is not int):
        return False
    contexts = next(row for row in rows if row.get('test') == 'translated_context_cleanup')
    if not _ordered(rows, (*disks, contexts, shutdown[0], {'result': 'passed'})):
        return False
    forbidden = ('stock_init_original_system_mounted', 'stock_init_original_partition_mounted',
                 'stock_init_original_metadata', 'stock_init_sdk_dm', 'stock_init_data_mount', 'stock_init_metadata_mount')
    if any(any(key in row for key in forbidden) for row in rows):
        return False
    restorer = [row for row in rows if row.get('test') == 'linux_signal_kernel_restorer']
    if restorer != [{'test': 'linux_signal_kernel_restorer', 'passed': True, 'pid': pid}
                    for pid in ((1, 2) if mode == 'lifecycle' else (1,))]:
        return False
    lifecycle = [row for row in rows if 'stock_init_lifecycle' in row]
    executed = [row for row in rows if 'stock_init_exec' in row]
    if mode == 'lifecycle':
        return (lifecycle == [{'stock_init_lifecycle': True, 'pid': 1, 'child': 2}] and not executed and
                observation['deadline_stop'] is True and observation['status'] == 9 and
                contexts['stock_init_exec_count'] == 0 and
                contexts['allocated'] == (3 if any('stock_init_owned_ordinary' in row for row in rows) else 2) and
                not any('stock_init_exec_layout' in row for row in rows))
    return (mode == 'original' and not lifecycle and
            executed == [{'stock_init_exec': True, 'pid': 1, 'path': '/init', 'arguments': []}] and
            bool(first_exec) and _ordered(rows, (cleanup, executed[0], output[0])) and
            validate_exec_layout(rows, inputs['exec_elf_images'], guest, page, init_paths=INIT_PATHS))


def progress(rows, inputs):
    sysfs = observe_devices(rows, inputs, True)
    block = [dict(row, row_index=index, paths=[bytes.fromhex(value).decode('utf-8', errors='replace') for value in row['paths_hex']])
             for index, row in enumerate(rows) if 'stock_init_block' in row]
    dynamic = [index for index, row in enumerate(rows) if row.get('stock_init_security') == 221 and
               row.get('result') == 0 and row.get('path_hex') == '/system/bin/init'.encode().hex()]
    exec_events = [(index, row) for index, row in enumerate(rows) if 'stock_init_exec_layout' in row or
                   (row.get('stock_init_security') == 221 and row.get('result') == 0 and
                    row.get('path_hex') in {path.encode().hex() for path in INIT_PATHS})]
    dynamic_pid_one = []
    for position, (index, security) in enumerate(exec_events[:-1]):
        _, layout = exec_events[position + 1]
        if (index in dynamic and layout.get('stock_init_exec_layout') is True and
                type(security.get('exec_index')) is int and security['exec_index'] > 0 and
                type(layout.get('exec_index')) is int and layout['exec_index'] == security['exec_index'] and
                type(layout.get('pid')) is int and layout['pid'] == 1 and
                type(layout.get('read_result')) is int and layout['read_result'] == 0):
            dynamic_pid_one.append(index)
    end = dynamic[0] if dynamic else len(rows)
    success = [row for row in block if row['pid'] == 1 and row['result'] == 0 and row['row_index'] < end]

    def devices_before(before):
        nodes, links = {}, {}
        for row in success:
            if row['row_index'] >= before:
                break
            operation, paths = row['stock_init_block'], row['paths']
            if operation == 33 and stat.S_IFMT(row['args'][2]) == stat.S_IFBLK:
                nodes[paths[0]] = {'path': paths[0], 'device': row['args'][3], 'row_index': row['row_index']}
                links.pop(paths[0], None)
            elif operation == 36:
                links[paths[1]] = paths[0]
                nodes.pop(paths[1], None)
            elif operation == 35:
                nodes.pop(paths[0], None)
                links.pop(paths[0], None)
        return nodes, links

    def resolve(path, before):
        nodes, links = devices_before(before)
        seen = set()
        while path in links and path not in seen:
            seen.add(path)
            path = links[path]
        node = nodes.get(path)
        return node if node and _uint(node['device'], 32) and node['device'] else None

    _, links = devices_before(end)
    super_by_name = '/dev/block/by-name/super'
    platform_super = '/dev/block/platform/a003c00.virtio_mmio/by-name/super'
    super_path = links.get(super_by_name)
    super_node = resolve(super_by_name, end)
    super_dev = super_node['device'] if super_node else None
    super_matches = (super_path == '/dev/block/ncedisk1p2' and links.get(platform_super) == super_path and
                     super_node is not None and super_dev == sysfs['ncedisk1p2']['device'])
    metadata_path = '/dev/block/platform/a003c00.virtio_mmio/by-name/metadata'
    metadata_node = links.get(metadata_path)
    metadata_device = resolve(metadata_path, end)
    metadata_matches = (metadata_node == '/dev/block/ncedisk2p1' and metadata_device is not None and
                        links.get('/dev/block/by-name/metadata') == metadata_node and
                        metadata_device['device'] == sysfs['ncedisk2p1']['device'])
    tables = [dict(row, row_index=index) for index, row in enumerate(rows)
              if row.get('stock_init_dm_table_load') is True and row['pid'] == 1 and index < end]
    mounts = [row for row in success if row['stock_init_block'] == 40]
    metadata = [row for row in mounts if row['paths'][1:] == ['/metadata', 'ext4']]
    metadata_mounted = []
    for row in metadata:
        source = resolve(row['paths'][0], row['row_index'])
        expected = resolve(metadata_path, row['row_index'])
        if (metadata_matches and source and expected and
                source['device'] == expected['device'] == metadata_device['device']):
            metadata_mounted.append(row)
    mounted = {name: [row for row in mounts if row['paths'][1:] == ['/' + name, expected['filesystem']]]
               for name, expected in inputs['sdk_partitions'].items()}
    lp = {}
    for name, expected in inputs['sdk_partitions'].items():
        matching = [row for row in tables if row['name_hex'] == name.encode().hex() and row['result'] == 0 and
                    row['read_result'] == 0 and len(row['targets']) == 1]
        valid = False
        mapper_path = '/dev/block/mapper/' + name
        mapper = resolve(mapper_path, end)
        if len(matching) == 1 and super_matches:
            table = matching[0]
            target = table['targets'][0]
            source = resolve(super_by_name, table['row_index'])
            backing = resolve(bytes.fromhex(target['backing_path_hex']).decode(), table['row_index']) if 'backing_path_hex' in target else None
            backing = backing['device'] if backing else None
            if 'backing_major' in target:
                backing = (target['backing_major'] << 8) | (target['backing_minor'] & 255) | ((target['backing_minor'] & ~255) << 12)
            valid = (target['type_hex'] == b'linear'.hex() and target['sector_start'] == 0 and
                     target['length'] == expected['sizelimit'] // 512 and
                     target.get('backing_start_sector') == (expected['offset'] - inputs['lp_metadata']['super_offset']) // 512 and
                     source is not None and backing == source['device'] == super_dev and table['flags'] & 1 == 1 and
                     bool(metadata_mounted) and metadata_mounted[0]['row_index'] < table['row_index'])
        mapped_mounts = []
        for row in mounted[name]:
            source = resolve(row['paths'][0], row['row_index'])
            node = resolve(mapper_path, row['row_index'])
            node_name = re.fullmatch(r'/dev/block/dm-(0|[1-9][0-9]*)', node['path']) if node else None
            minor = ((node['device'] & 255) | ((node['device'] >> 12) & 0xfff00)) if node else None
            if (valid and source and mapper and node_name and source['device'] == node['device'] == mapper['device'] and
                    int(node_name[1]) == minor and row['args'][3] & 1 and
                    matching[0]['row_index'] < row['row_index']):
                mapped_mounts.append(row)
        lp[name] = {'table_loads': matching, 'matches_original_lp': valid,
                    'mapper_path': mapper_path, 'mapper_node': mapper,
                    'mounts_match_device': bool(mapped_mounts), 'matched_mounts': mapped_mounts}
    root_moves = [row for row in mounts if row['paths'] == ['/system', '/', ''] and row['args'][3] == 8192]
    chdirs = [row for row in success if row['stock_init_block'] == 49 and row['paths'] == ['/system']]
    chroots = [row for row in success if row['stock_init_block'] == 51 and row['paths'] == ['.']]
    root_changed = bool(chdirs and root_moves and chroots and
                        chdirs[0]['row_index'] < root_moves[0]['row_index'] < chroots[0]['row_index'])
    removed = [row for row in success if row['stock_init_block'] == 35 and row['paths'][0] in ('init', '/init')]
    mapped_devices = [item['mapper_node']['device'] for item in lp.values() if item['mapper_node']]
    mapper_majors = {(device >> 8) & 4095 for device in mapped_devices}
    mapper_class_valid = (len(mapped_devices) == len(inputs['sdk_partitions']) and len(mapper_majors) == 1 and
                          not mapper_majors & {0, sysfs['ncedisk1']['major'], sysfs['ncedisk1p2']['major']})
    system_mounts = lp['system']['matched_mounts']
    mounted_before_exec = all(item['matched_mounts'] and item['matched_mounts'][0]['row_index'] < end for item in lp.values())
    mounted_after_root = bool(chroots) and all(name == 'system' or
                                             (item['matched_mounts'] and chroots[0]['row_index'] < item['matched_mounts'][0]['row_index'])
                                             for name, item in lp.items())
    completed = (metadata_matches and bool(metadata_mounted) and super_matches and
                 all(item['matches_original_lp'] and item['mounts_match_device'] for item in lp.values()) and
                 mapper_class_valid and len(set(mapped_devices)) == len(inputs['sdk_partitions']) and root_changed and
                 bool(system_mounts) and system_mounts[0]['row_index'] < chdirs[0]['row_index'] and
                 mounted_after_root and mounted_before_exec and bool(dynamic_pid_one) and chroots[0]['row_index'] < dynamic_pid_one[0])
    return {'metadata_by_name': {'path': metadata_path, 'target': metadata_node, 'created': metadata_matches,
                                 'device': metadata_device},
            'metadata_mounts': metadata, 'matched_metadata_mounts': metadata_mounted,
            'super_path': super_path, 'super_device': super_dev, 'super_matches_input': super_matches,
            'logical_partitions': lp, 'partition_mounts': mounted, 'switch_root': root_changed,
            'partition_mounts_after_switch_root': mounted_after_root,
            'mapper_device_class_valid': mapper_class_valid,
            'root_move_calls': root_moves, 'chroot_calls': chroots, 'ramdisk_init_unlinks': removed,
            'dynamic_init_execs': len(dynamic), 'dynamic_init_pid_one_execs': len(dynamic_pid_one), 'completed': completed}


def require_complete(report, lifecycle_only):
    if lifecycle_only:
        return
    runs = report.get('runs', [])
    if type(runs) is not list or any(type(run) is not dict for run in runs):
        raise ValueError('原 /init 的 first-stage 运行记录格式错误')
    original = [run for run in runs if run.get('mode') == 'original']
    if (report.get('first_stage_completed') is not True or len(original) != 2 or
            any(type(run.get('entry')) is not str for run in original) or
            sorted(run.get('entry', '') for run in original) != ['cli', 'gcd'] or
            any(run.get('phase') != 'original' or run.get('observation_validated') is not True or
                type(run.get('first_stage')) is not dict or
                run.get('first_stage', {}).get('completed') is not True for run in original)):
        raise ValueError('原 /init 的 first-stage 未完成')
