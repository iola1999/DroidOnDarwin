"""生成固定原系统服务观察器，并验收同一 boot_id 的已导出记录。"""
import hashlib
import json
import os
from pathlib import Path
import re
import stat

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'experiments/native-linux/translation'
HEADER_NAME = 'stock-init-system-observer-data.h'
MANIFEST_NAME = 'stock-init-system-observer.json'
WRAPPER_PATH = '/dev/droid-plain-init-wrapper.rc'
RC_PATH = '/dev/droid-plain-system-observer.rc'
CMDLINE_OPTION = 'androidboot.init_rc=' + WRAPPER_PATH
FORMAT = 'droid_plain_system_observer_v1'
CONTRACT = 2
_SELINUX = 'u:object_r:tmpfs:s0'
_SCRIPT_TOKEN = '__DROID_PLAIN_SYSTEM_OBSERVER_SCRIPT__'
_MAX_PAYLOAD_BYTES = 64 << 10
_MAX_EXPORT_BYTES = 32 << 20
_MAX_TSV_BYTES = 64 << 10
_BOOT_ID = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
_BOOT_IMPORTS = (
    '/system/etc/init/hw/init.rc',
    '/system/etc/init',
    '/system_ext/etc/init',
    '/vendor/etc/init',
    '/odm/etc/init',
    '/product/etc/init',
    RC_PATH,
)
_CHECK_COMMANDS = (
    ('boot_completed_begin', '/system/bin/getprop sys.boot_completed'),
    ('zygote_state', '/system/bin/getprop init.svc.zygote'),
    ('surfaceflinger_state', '/system/bin/getprop init.svc.surfaceflinger'),
    ('zygote64_pid', '/system/bin/pidof zygote64'),
    ('system_server_pid', '/system/bin/pidof system_server'),
    ('surfaceflinger_pid', '/system/bin/pidof surfaceflinger'),
    ('binder_activity', '/system/bin/service check activity'),
    ('binder_package', '/system/bin/service check package'),
    ('binder_window', '/system/bin/service check window'),
    ('binder_input', '/system/bin/service check input'),
    ('binder_lock_settings', '/system/bin/service check lock_settings'),
    ('binder_SurfaceFlinger', '/system/bin/service check SurfaceFlinger'),
    ('cmd_activity', '/system/bin/cmd activity get-current-user'),
    ('cmd_package', '/system/bin/cmd package path android'),
    ('cmd_window', '/system/bin/cmd window size'),
    ('cmd_input', '/system/bin/cmd input help'),
    ('cmd_lock_settings', '/system/bin/cmd lock_settings get-disabled --user 0'),
    ('boot_id_end', '/system/bin/cat /proc/sys/kernel/random/boot_id'),
    ('boot_completed_end', '/system/bin/getprop sys.boot_completed'),
)


def _read_bytes(path, limit):
    try:
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
            raise ValueError('观察器文件类型或大小不同：' + str(path))
        with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW), 'rb') as source:
            current = os.fstat(source.fileno())
            if not stat.S_ISREG(current.st_mode) or current.st_size > limit:
                raise ValueError('观察器文件读取前的类型或大小不同：' + str(path))
            data = source.read(limit + 1)
    except OSError as error:
        raise ValueError('观察器文件读取失败：' + str(path)) from error
    if len(data) > limit:
        raise ValueError('观察器文件读取期间超过限额：' + str(path))
    return data


def _decode(data, path):
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        raise ValueError('观察器文本编码不同：' + str(path)) from None
    if '\x00' in text or '\r' in text:
        raise ValueError('观察器文本包含无效分隔字符：' + str(path))
    return text


def _shell_token(data, path):
    text = _decode(data, path)
    if not text:
        raise ValueError('观察器脚本为空：' + str(path))
    # 正文使用未加外层引号的单个 token；service exec 前的 ExpandProps 还原字面 $。
    escapes = {'\\': '\\\\', ' ': '\\ ', '\t': '\\t', '\n': '\\n',
               '"': '\\"', '#': '\\#', '$': '$$'}
    return ''.join(escapes.get(character, character) for character in text)


def prepare(output):
    """生成两份固定 /dev rc 的 header 和清单；同名输出已存在时失败。"""
    output = Path(output).resolve()
    header, manifest_path = output / HEADER_NAME, output / MANIFEST_NAME
    if any(path.exists() or path.is_symlink() for path in (header, manifest_path)):
        raise ValueError('原系统服务观察器输出已存在')
    script, rc = ASSETS / 'stock-init-system-observer.sh', ASSETS / 'stock-init-system-observer.rc'
    script_bytes = _read_bytes(script, _MAX_PAYLOAD_BYTES)
    rc_text = _decode(_read_bytes(rc, _MAX_PAYLOAD_BYTES), rc)
    if rc_text.count(_SCRIPT_TOKEN) != 1:
        raise ValueError('观察器 rc 的固定命令参数不同')
    rc_bytes = rc_text.replace(_SCRIPT_TOKEN, _shell_token(script_bytes, script)).encode()
    wrapper = ''.join('import ' + path + '\n' for path in _BOOT_IMPORTS).encode()
    payloads = (
        (WRAPPER_PATH, wrapper, str(Path(__file__).resolve())),
        (RC_PATH, rc_bytes, str(rc)),
    )
    if any(not data for _, data, _ in payloads) or sum(len(data) for _, data, _ in payloads) > _MAX_PAYLOAD_BYTES:
        raise ValueError('固定观察器 payload 为空或超过 64 KiB')
    records, blob = [], bytearray()
    for path, data, source in payloads:
        records.append({'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                        'mode': 0o644, 'uid': 0, 'gid': 0, 'selinux': _SELINUX,
                        'source': source, 'offset': len(blob)})
        blob.extend(data)
    lines = [
        '/* 固定 SDK36 实验观察器；仅在原 first-stage 的实际 /dev 挂载内部署。 */',
        '#pragma once',
        'struct stock_init_system_observer_file {',
        '    const char *path;',
        '    const unsigned char *data;',
        '    unsigned long bytes;',
        '    unsigned mode, uid, gid;',
        '    const char *selinux;',
        '};',
        'static const unsigned char stock_init_system_observer_data[] = {',
    ]
    for offset in range(0, len(blob), 24):
        lines.append('    ' + ','.join(f'0x{byte:02x}' for byte in blob[offset:offset + 24]) + ',')
    lines += ['};', 'static const struct stock_init_system_observer_file stock_init_system_observer_files[] = {']
    for record in records:
        lines.append('    {%s, stock_init_system_observer_data + %d, %d, 0644, 0, 0, %s},' %
                     (json.dumps(record['path']), record['offset'], record['bytes'], json.dumps(_SELINUX)))
    lines += ['};', 'enum { STOCK_INIT_SYSTEM_OBSERVER_FILE_COUNT =',
              '    sizeof(stock_init_system_observer_files) / sizeof(stock_init_system_observer_files[0]) };']
    header_bytes = ('\n'.join(lines) + '\n').encode()
    manifest = {'contract': CONTRACT, 'format': FORMAT, 'cmdline_option': CMDLINE_OPTION,
                'header': str(header), 'header_sha256': hashlib.sha256(header_bytes).hexdigest(),
                'manifest': str(manifest_path), 'payload_bytes': len(blob), 'payloads': records,
                'boot_imports': list(_BOOT_IMPORTS), 'checks': [name for name, _ in _CHECK_COMMANDS],
                'source_module_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'script_source': str(script), 'script_source_sha256': hashlib.sha256(script_bytes).hexdigest()}
    output.mkdir(parents=True, exist_ok=True)
    with header.open('xb') as stream:
        stream.write(header_bytes)
    with manifest_path.open('x') as stream:
        stream.write(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    return manifest


def _content_valid(name, text, boot_id):
    if not text.endswith('\n'):
        return False
    lines = text.splitlines()
    if name in ('boot_completed_begin', 'boot_completed_end'):
        return lines == ['1']
    if name in ('zygote_state', 'surfaceflinger_state'):
        return lines == ['running']
    if name.endswith('_pid'):
        return len(lines) == 1 and re.fullmatch(r'[1-9][0-9]*(?: +[1-9][0-9]*)*', lines[0]) is not None
    if name.startswith('binder_'):
        return lines == ['Service ' + name[len('binder_'):] + ': found']
    if name == 'cmd_activity':
        return len(lines) == 1 and re.fullmatch(r'[0-9]+', lines[0]) is not None
    if name == 'cmd_package':
        return bool(lines) and all(re.fullmatch(r'package:/[^\s]+', line) is not None for line in lines)
    if name == 'cmd_window':
        return (1 <= len(lines) <= 2 and
                re.fullmatch(r'Physical size: [1-9][0-9]*x[1-9][0-9]*', lines[0]) is not None and
                (len(lines) == 1 or re.fullmatch(r'Override size: [1-9][0-9]*x[1-9][0-9]*', lines[1]) is not None))
    if name == 'cmd_input':
        return bool(lines) and lines[0].startswith('Usage: input ')
    if name == 'cmd_lock_settings':
        return lines in (['true'], ['false'])
    if name == 'boot_id_end':
        return lines == [boot_id]
    return False


def observe_deployment(rows, manifest, *, required):
    """核对原 PID 1 的实际部署及 exec 后读回；返回本轮 boot_id。"""
    records = [(index, row) for index, row in enumerate(rows)
               if 'stock_init_system_observer_file' in row or 'stock_init_system_observer_ready' in row]
    if not required:
        if records:
            raise ValueError('当前入口包含额外的原系统观察器部署')
        return {}
    if type(manifest.get('contract')) is not int or manifest['contract'] != CONTRACT or len(manifest.get('payloads', [])) != 2:
        raise ValueError('原系统服务观察器部署清单不同')
    raw_init = [index for index, row in enumerate(rows)
                if row.get('stock_init_exec_layout') is True and type(row.get('pid')) is int and row['pid'] == 1 and
                type(row.get('exec_index')) is int and row['exec_index'] == 1]
    if len(raw_init) != 1:
        raise ValueError('原 /init 的执行记录缺失或重复')
    proc_mounts = [index for index, row in enumerate(rows)
                   if index > raw_init[0] and type(row.get('stock_init_block')) is int and row['stock_init_block'] == 40 and
                   type(row.get('pid')) is int and row['pid'] == 1 and
                   type(row.get('result')) is int and row['result'] == 0 and
                   row.get('paths_hex', [])[1:] == ['/proc'.encode().hex(), 'proc'.encode().hex()]]
    ready = [(index, row) for index, row in records if 'stock_init_system_observer_ready' in row]
    if not proc_mounts or not ready or ready[0][1].get('phase') != 'deploy':
        raise ValueError('原 PID 1 的 /proc 挂载或观察器部署缺失')
    boot_id = ready[0][1].get('boot_id')
    if type(boot_id) is not str or _BOOT_ID.fullmatch(boot_id) is None:
        raise ValueError('部署读取的内核 boot_id 不同')
    execs = [(index, row.get('exec_index')) for index, row in enumerate(rows)
             if row.get('stock_init_exec_layout') is True and type(row.get('pid')) is int and row['pid'] == 1 and index > ready[0][0]]
    if (any(type(value) is not int or not 1 < value < 1 << 64 for _, value in execs) or
            any(before >= after for (_, before), (_, after) in zip(execs, execs[1:]))):
        raise ValueError('原 PID 1 的 exec 索引类型或顺序不同')
    if len(execs) < 2 or [value for _, value in execs[:2]] != [2, 3]:
        raise ValueError('固定 SDK36 的策略加载和 second_stage 执行记录不同')
    third_exec = [index for index, row in enumerate(rows)
                  if type(row.get('stock_init_security')) is int and row['stock_init_security'] == 221 and
                  type(row.get('result')) is int and row['result'] == 0 and
                  type(row.get('exec_index')) is int and row['exec_index'] == 3 and
                  row.get('path_hex') == '/system/bin/init'.encode().hex()]
    if len(third_exec) != 1 or not execs[0][0] < third_exec[0] < execs[1][0]:
        raise ValueError('原 second_stage 的 exec 路径或执行边界不同')
    groups = [('deploy', 0, proc_mounts[0]), *(('exec', exec_index, index) for index, exec_index in execs)]
    if len(records) != 3 * len(groups):
        raise ValueError('观察器 exec 后的文件读回缺失或重复')
    ready_fields = {'stock_init_system_observer_ready', 'phase', 'exec_index', 'pid', 'boot_id', 'files', 'scratch_unmapped'}
    file_fields = {'stock_init_system_observer_file', 'phase', 'exec_index', 'pid', 'path', 'bytes', 'mode',
                   'uid', 'gid', 'nlink', 'label_hex', 'content_matches'}
    previous = proc_mounts[0]
    for group_index, (phase, exec_index, trigger_index) in enumerate(groups):
        group = records[3 * group_index:3 * group_index + 3]
        for (index, row), payload in zip(group[:2], manifest['payloads']):
            expected = {'stock_init_system_observer_file': True, 'phase': phase, 'exec_index': exec_index, 'pid': 1,
                        'path': payload['path'], 'bytes': payload['bytes'], 'mode': 0o100000 | payload['mode'],
                        'uid': payload['uid'], 'gid': payload['gid'], 'nlink': 1,
                        'label_hex': (payload['selinux'].encode() + b'\0').hex() if exec_index >= 3 else '',
                        'content_matches': True}
            if (set(row) != file_fields or row != expected or
                    row['stock_init_system_observer_file'] is not True or row['content_matches'] is not True or
                    any(type(row[key]) is not int for key in ('exec_index', 'pid', 'bytes', 'mode', 'uid', 'gid', 'nlink')) or
                    index <= max(previous, trigger_index)):
                raise ValueError('观察器文件内容、标签或部署顺序不同：' + payload['path'])
            previous = index
        index, row = group[2]
        expected = {'stock_init_system_observer_ready': True, 'phase': phase, 'exec_index': exec_index, 'pid': 1,
                    'boot_id': boot_id, 'files': 2, 'scratch_unmapped': True}
        next_boundary = (third_exec[0] if exec_index == 2 else
                         groups[group_index + 1][2] if group_index + 1 < len(groups) else None)
        if (set(row) != ready_fields or row != expected or row['stock_init_system_observer_ready'] is not True or
                row['scratch_unmapped'] is not True or
                any(type(row[key]) is not int for key in ('exec_index', 'pid', 'files')) or index <= previous or
                (next_boundary is not None and index >= next_boundary)):
            raise ValueError('观察器身份、临时映射回收或完成顺序不同')
        previous = index
    return {'boot_id': boot_id, 'deployed': True, 'verified_exec_indices': [value for _, value in execs],
            'payloads': len(manifest['payloads'])}


def validate_export(export_dir, boot_id):
    """验收已停止并导出的本轮目录；完整失败返回 passed=False，格式或内容矛盾抛 ValueError。"""
    if type(boot_id) is not str or _BOOT_ID.fullmatch(boot_id) is None:
        raise ValueError('原系统服务观察器 boot_id 格式不同')
    directory = Path(export_dir).resolve()
    if not directory.is_dir() or directory.name != boot_id:
        raise ValueError('观察器导出目录与本轮 boot_id 不同')
    incomplete = directory / 'result.incomplete.tsv'
    if incomplete.exists() or incomplete.is_symlink():
        raise ValueError('原系统服务观察器尚未完成')
    finished = [directory / ('result.' + result + '.tsv') for result in ('passed', 'failed')
                if (directory / ('result.' + result + '.tsv')).exists() or
                (directory / ('result.' + result + '.tsv')).is_symlink()]
    if len(finished) != 1:
        raise ValueError('观察器需要唯一的完整结果文件')
    result_path = finished[0]
    data = _read_bytes(result_path, _MAX_TSV_BYTES)
    text = _decode(data, result_path)
    rows = [line.split('\t') for line in text.splitlines()]
    expected_header = [['format', FORMAT], ['kernel_boot_id', boot_id],
                       ['command_seconds', '5'], ['kill_after_seconds', '1'], ['observer_begin']]
    if not text.endswith('\n') or len(rows) != 6 + 2 * len(_CHECK_COMMANDS) or rows[:5] != expected_header:
        raise ValueError('观察器 TSV 首部、行数或结束换行不同')
    files = {result_path.name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}}
    total_bytes = len(data)
    checks = []
    for index, (name, command) in enumerate(_CHECK_COMMANDS):
        command_row, check = rows[5 + 2 * index:7 + 2 * index]
        if command_row != ['command', name, command] or len(check) != 6 or check[:2] != ['check', name]:
            raise ValueError('观察器命令顺序或检查字段不同：' + name)
        if (re.fullmatch(r'0|[1-9][0-9]{0,2}', check[2]) is None or int(check[2]) > 255 or
                check[3] not in ('passed', 'failed') or check[4:] != [name + '.stdout', name + '.stderr']):
            raise ValueError('观察器返回码、状态或文件名不同：' + name)
        returncode = int(check[2])
        output_text = None
        for filename in check[4:]:
            path = directory / filename
            data = _read_bytes(path, _MAX_EXPORT_BYTES - total_bytes)
            total_bytes += len(data)
            files[filename] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            if filename.endswith('.stdout'):
                output_text = _decode(data, path)
        content_valid = _content_valid(name, output_text, boot_id)
        outcome = 'passed' if returncode == 0 and content_valid else 'failed'
        if check[3] != outcome:
            raise ValueError('观察器查询内容与检查状态不同：' + name)
        checks.append({'name': name, 'returncode': returncode, 'outcome': outcome, 'content_valid': content_valid})
    failed_checks = [check['name'] for check in checks if check['outcome'] == 'failed']
    result = 'failed' if failed_checks else 'passed'
    if (rows[-1] != ['observer_end', 'complete', result, str(len(checks)), str(len(failed_checks))] or
            result_path.name != 'result.' + result + '.tsv'):
        raise ValueError('观察器完整结束记录或结果文件名不同')
    for filename in ('finalize-sync.stderr', 'finalize-mv.stderr'):
        data = _read_bytes(directory / filename, _MAX_EXPORT_BYTES - total_bytes)
        total_bytes += len(data)
        if data:
            raise ValueError('观察器最终写入产生 stderr：' + filename)
        files[filename] = {'bytes': 0, 'sha256': hashlib.sha256(data).hexdigest()}
    return {'boot_id': boot_id, 'complete': True, 'passed': result == 'passed',
            'result': result, 'checks': checks, 'failures': len(failed_checks), 'failed_checks': failed_checks,
            'result_file': str(result_path), 'files': files, 'export_bytes': total_bytes}
