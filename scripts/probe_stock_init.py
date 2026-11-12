#!/usr/bin/env python3
"""在已验证的 sparse 构建上观察原始 SDK36 init，先检查 PID 1 停止与回收。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
RUNTIME_SHA = '4b1a45b0ee51725c68f1c29765958d0b60b45a5f2325bedfc61764cd49232249'
DESKTOP_SHA = '515c9fe37032c16e01048a9a63f83708b314558d5322de2eb41773db09179772'
ROOT_FILES = {'/init.environ.rc': '460eecf019489a02ad7be82a9cc2234784f561b644f8f2bf1741e43514593060',
              '/system/build.prop': 'e7ece731169cc37c759dd827c206a0c3c3c24fde6937dd96982a4b83d70dfc66'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare(runtime, desktop, metadata, out):
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
        shutil.copyfile(path, target)
        mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
        target.chmod(mode)
        records[guest] = {'source': str(path), 'sha256': expected, 'bytes': path.stat().st_size, 'mode': mode}
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
    directories = {str(parent) for guest in records for parent in Path(guest).parents if str(parent) != '/'}
    lines = ['D\t' + name for name in sorted(directories, key=lambda name: (name.count('/'), name))]
    lines += ['F\t%o\t%s' % (item['mode'], guest) for guest, item in records.items()]
    (out / 'files.tsv').write_text('\n'.join(lines) + '\n')
    return {'runtime_manifest': str(runtime_manifest), 'runtime_manifest_sha256': RUNTIME_SHA,
            'desktop_manifest': str(desktop_manifest), 'desktop_manifest_sha256': DESKTOP_SHA,
            'closure': closure, 'files': records,
            'limitations': ['Original rc files retained; service binaries beyond init/ueventd and APEX containers are not staged.',
                            'The runner supplies mounted partition directories; first-stage init and original fstab mounts are untested.']}


def validate(rows, mode):
    observations = [r for r in rows if 'stock_init_observation' in r]
    cleanup = [r for r in rows if r.get('test') == 'translated_context_cleanup']
    logs = [r for r in rows if 'stock_init_kernel_log' in r]
    endings = [r for r in rows if 'result' in r and 'component_vm' not in r and 'stock_init_write' not in r]
    if len(observations) != 1 or len(cleanup) != 1 or len(logs) != 1 or endings != [{'result': 'passed'}]:
        return False
    o, c, log = observations[0], cleanup[0], logs[0]
    if (set(o) != {'stock_init_observation', 'mode', 'ancestor_pid', 'waited', 'status', 'deadline_stop', 'kill_result'} or
            o['stock_init_observation'] is not True or o['mode'] != mode or type(o['deadline_stop']) is not bool or
            any(type(o[key]) is not int for key in ('ancestor_pid', 'waited', 'status', 'kill_result')) or
            o['ancestor_pid'] <= 1 or o['waited'] != o['ancestor_pid'] or not 0 <= o['status'] <= 65535 or
            o['kill_result'] != 0 or (o['deadline_stop'] and o['status'] != 9) or
            c.get('passed') is not True or type(c.get('allocated')) is not int or c['allocated'] <= 0 or
            type(c.get('freed')) is not int or c['allocated'] != c['freed'] or
            set(log) != {'stock_init_kernel_log', 'read_result'} or log['stock_init_kernel_log'] is not True or
            type(log['read_result']) is not int or not -4095 <= log['read_result'] < 1 << 20):
        return False
    writes = [r for r in rows if 'stock_init_write' in r]
    for row in writes:
        if (set(row) != {'stock_init_write', 'fd', 'result', 'vector', 'bytes', 'captured', 'hex'} or
                any(type(row[key]) is not int for key in ('stock_init_write', 'fd', 'result', 'vector', 'bytes', 'captured')) or
                row['stock_init_write'] not in (64, 66) or row['fd'] < 0 or not 0 <= row['vector'] < 8 or
                not 0 < row['captured'] <= min(row['bytes'], 2048) or type(row['hex']) is not str or
                re.fullmatch('[0-9a-f]{%d}' % (2 * row['captured']), row['hex']) is None):
            return False
    lifecycle = [r for r in rows if 'stock_init_lifecycle' in r]
    original = [r for r in rows if 'stock_init_exec' in r]
    if mode == 'lifecycle':
        return (lifecycle == [{'stock_init_lifecycle': True, 'pid': 1, 'child': 2}] and not original and
                o['deadline_stop'] and o['status'] == 9 and c['allocated'] == 2 and log['read_result'] >= 0)
    paths = [r.get('path') for r in rows if r.get('phase') == 'runtime_elf']
    return (not lifecycle and original == [{'stock_init_exec': True, 'pid': 1, 'argument': 'selinux_setup'}] and
            paths.count('/system/bin/init') == 1 and paths.count('/system/bin/bootstrap/linker64') == 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--desktop', type=Path, required=True)
    parser.add_argument('--root-metadata', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = args.report.resolve()
    base = json.loads(source.read_text())
    config = base['configuration']
    if (config['guest_layout'] != 'sparse' or config['workload'] != 'core' or config['cpus'] != 1 or
            config['execution'] != 'direct' or config['direct']['memory'] != 'flat' or not config['data_tbi'] or
            config.get('guest_page_bytes', 16384) != 16384 or config['mapping'] != 'pte' or config['code_memory'] != 'dual' or
            not base['runs'] or not all(r['validated'] for r in base['runs'])):
        parser.error('需要成功的 sparse 16 KiB 单 CPU Direct flat/TBI 核心构建')
    for name, expected in base['sha256'].items():
        if sha(ROOT / name) != expected:
            raise ValueError('基准输入变化：' + name)
    out = args.output.resolve()
    if not out.is_relative_to(ROOT / 'work/native-linux-nce'):
        parser.error('输出需要位于独立的 work/native-linux-nce 子目录')
    out.mkdir(parents=True, exist_ok=False)
    prepared = prepare(args.runtime.resolve(), args.desktop.resolve(), args.root_metadata.resolve(), out)
    result = {key: base[key] for key in ('platform', 'cpu', 'system_build', 'dependency')}
    result.update(scope='Original SDK36 init selinux_setup/second_stage observation; full rc retained; first-stage and system boot unverified',
                  source_report=str(source), source_report_sha256=sha(source), source_configuration=config,
                  input=prepared, commands=[], runs=[], sha256=dict(base['sha256']))
    for path in (Path(__file__).resolve(), EXP / 'translation/stock-init-guest.c', EXP / 'translation/stock-init-runner.h', EXP / 'translation/stock-init-trace.h'):
        result['sha256'][str(path.relative_to(ROOT))] = sha(path)
    runner_source = (EXP / 'translation/linux-runner.c').read_text()
    start = runner_source.index('static void stock_bionic_setup(void)')
    end = runner_source.index('#include "stock-art-runner.h"', start)
    runner_source = runner_source[:start] + runner_source[end:]
    original = '    } else if (stock_bionic) {\n        stock_bionic_setup();\n        translation_memory_phase("guest_execution");\n        failed = run_program("/probe-guest", 1);'
    if runner_source.count(original) != 1:
        raise ValueError('运行器组件入口已变化')
    runner_source = runner_source.replace(original, '    } else if (stock_bionic) {\n        stock_init_setup();\n        translation_memory_phase("guest_execution");\n        failed = stock_init_run();')
    runner_source = runner_source.replace('static int detach_preparation(void)', '#include "stock-init-runner.h"\n\nstatic int detach_preparation(void)')
    runner = out / 'linux-runner.c'
    runner.write_text(runner_source)
    result['sha256'][str(runner.relative_to(ROOT))] = sha(runner)
    execution = out / 'linux-execution.c'
    execution_text = (EXP / 'translation/linux-execution.c').read_text()
    execution_text = execution_text.replace('static void user_run(void *opaque)', '#include "stock-init-trace.h"\n\nstatic void user_run(void *opaque)')
    before = '            long result = run_syscall(number, args);'
    if execution_text.count(before) != 1:
        raise ValueError('syscall 观察位置已变化')
    execution_text = execution_text.replace(before, before + '\n            stock_init_trace_write(number, args, result);')
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
    for command in (compile_guest, link_guest):
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
    result['test_environment'] = selected
    for entry in ('cli', 'gcd'):
        command = next(list(cmd) for cmd in base['commands'] if '-o' in cmd and Path(cmd[cmd.index('-o') + 1]).name == entry)
        includes = [command[i + 1] for i, value in enumerate(command[:-1]) if value == '-I']
        kernel = next(Path(path) / 'lkl-image.bin' for path in includes if (Path(path) / 'lkl-image.bin').is_file())
        command[command.index(str(EXP / 'translation/linux-runner.c'))] = str(runner)
        command[command.index(str(EXP / 'translation/linux-execution.c'))] = str(execution)
        command[command.index('-o') + 1] = str(out / entry)
        command += ['-iquote', str(EXP / 'translation')]
        subprocess.run(command, check=True, timeout=120)
        result['commands'].append(command)
        result['sha256'][str((out / entry).relative_to(ROOT))] = sha(out / entry)
    for mode in ('lifecycle', 'original'):
        for entry in ('cli', 'gcd'):
            env = {**{key: value for key, value in os.environ.items() if not key.startswith('NCE_')}, **selected, 'NCE_STOCK_INIT_MODE': mode}
            label = entry + '-' + mode
            with (out / (label + '.jsonl')).open('w') as stdout, (out / (label + '.log')).open('w') as stderr:
                try:
                    proc = subprocess.run([out / entry, kernel], env=env, stdout=stdout, stderr=stderr, timeout=90)
                    returncode = proc.returncode
                except subprocess.TimeoutExpired:
                    returncode = 'timeout'
            rows = [json.loads(line) for line in (out / (label + '.jsonl')).read_text().splitlines() if line.startswith('{')]
            stderr = (out / (label + '.log')).read_text()
            valid = returncode == 0 and validate(rows, mode) and 'stock_init_exec_errno' not in stderr
            guest_writes = ''.join(bytes.fromhex(row['hex']).decode('utf-8', errors='replace') for row in rows
                                   if 'stock_init_write' in row and bytes.fromhex(row['hex']).startswith(b'<'))
            result['runs'].append({'entry': entry, 'mode': mode, 'returncode': returncode, 'observation_validated': valid,
                                   'results': rows, 'stderr': stderr, 'guest_write_text': guest_writes,
                                   'attempted_log_messages': {text: text in stderr or text in guest_writes for text in ('Loading SELinux policy', 'init second stage started!', 'SetupMountNamespaces done')}})
            (out / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
            print(label, 'observed=' + str(valid), flush=True)
            if not valid:
                raise SystemExit('init 观察或回收检查失败：' + str(out / 'summary.json'))
    if any(sha(ROOT / name) != expected for name, expected in result['sha256'].items()):
        raise ValueError('运行期间构建输入变化')
    if any(sha(Path(item['source'])) != item['sha256'] for item in prepared['files'].values()):
        raise ValueError('运行期间原始输入变化')
    print(out / 'summary.json')


if __name__ == '__main__':
    main()
