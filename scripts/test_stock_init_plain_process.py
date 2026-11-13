#!/usr/bin/env python3
"""检查 plain init 观察的宿主进程回收及输出路径，测试仅创建宿主进程和小文件。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from types import SimpleNamespace
from unittest.mock import patch

sys.dont_write_bytecode = True
import probe_stock_init as probe
import process_observer

ROOT = Path(__file__).resolve().parents[1]
REAL_POPEN = subprocess.Popen
REAL_CHECK_OUTPUT = subprocess.check_output
REAL_KILLPG = os.killpg
FIXTURE = r'''
import json, os, signal, sys, time
from pathlib import Path
mode, ready_path = sys.argv[1:]
reader, writer = os.pipe()
child = os.fork()
if child == 0:
    os.close(reader)
    if 'ignore-term' in mode:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    os.write(writer, b'r')
    os.close(writer)
    time.sleep(60)
    os._exit(0)
os.close(writer)
assert os.read(reader, 1) == b'r'
os.close(reader)
Path(ready_path).write_text(json.dumps({'parent': os.getpid(), 'child': child, 'group': os.getpgrp()}))
if mode.startswith('leader-exit'):
    os._exit(0)
time.sleep(60)
'''


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def group_members(group):
    rows = REAL_CHECK_OUTPUT(['ps', '-axo', 'pid=,pgid='], text=True, timeout=10)
    return [int(fields[0]) for row in rows.splitlines()
            if len(fields := row.split()) == 2 and int(fields[1]) == group]


def process_case(output, name, mode, *, failed_query=None, stale_query=False, cleanup_interrupted=False, initial_interrupted=False):
    directory = output / name
    directory.mkdir()
    ready = directory / 'ready.json'
    command = [sys.executable, '-c', FIXTURE, mode, str(ready)]
    process = None
    fixture_wait = None
    queries = 0
    injected = False
    after_term = False
    record = {'name': name, 'passed': False, 'command': command}

    def launch(*args, **kwargs):
        nonlocal process, fixture_wait
        created = REAL_POPEN(*args, **kwargs)
        if list(args[0]) != command:
            return created
        process = created
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                identity = json.loads(ready.read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                time.sleep(0.01)
                continue
            if identity['parent'] != process.pid or identity['group'] != process.pid:
                raise ValueError('测试进程组与实际 leader 不一致')
            record['fixture'] = identity
            if initial_interrupted:
                fixture_wait = created.wait
                created.wait = interrupted_wait
            return created
        raise TimeoutError('宿主父子进程没有完成准备')

    def interrupted_wait(*args, **kwargs):
        nonlocal injected
        if not injected:
            injected = True
            os.kill(os.getpid(), signal.SIGTERM)
        return fixture_wait(*args, **kwargs)

    def enumerate_processes(command, **kwargs):
        nonlocal queries, injected
        if list(command) != ['ps', '-axo', 'pid=,pgid=']:
            return REAL_CHECK_OUTPUT(command, **kwargs)
        queries += 1
        if queries == failed_query:
            injected = True
            raise subprocess.CalledProcessError(1, command, stderr='injected ps failure')
        rows = REAL_CHECK_OUTPUT(command, **kwargs)
        if stale_query and queries == 1:
            if process is None or not group_members(process.pid):
                raise ValueError('信号竞态测试缺少实际子进程')
            REAL_KILLPG(process.pid, signal.SIGTERM)
            deadline = time.monotonic() + 3
            while group_members(process.pid) and time.monotonic() < deadline:
                time.sleep(0.01)
            if group_members(process.pid):
                raise TimeoutError('信号竞态测试的进程组尚未退出')
            injected = True
        return rows

    def kill_group(group, sig):
        nonlocal after_term
        REAL_KILLPG(group, sig)
        if cleanup_interrupted and sig == signal.SIGTERM:
            after_term = True

    def observed_time():
        nonlocal injected
        if after_term and not injected:
            injected = True
            # 子组已收到真实 SIGTERM，观察器的停止请求发生在清理计时步骤。
            os.kill(os.getpid(), signal.SIGTERM)
        return time.monotonic()

    started = time.monotonic()
    try:
        with (directory / 'stdout.log').open('w') as stdout, (directory / 'stderr.log').open('w') as stderr:
            with patch.object(process_observer.subprocess, 'Popen', launch), patch.object(process_observer.subprocess, 'check_output', enumerate_processes), \
                    patch.object(process_observer.os, 'killpg', kill_group), \
                    patch.object(process_observer, 'time', SimpleNamespace(monotonic=observed_time, sleep=time.sleep)):
                try:
                    record['observation'] = probe._observe(command, os.environ.copy(), stdout, stderr, 0.25)
                except Exception as error:
                    record['observation_error'] = {'type': type(error).__name__, 'message': str(error)}
                finally:
                    if fixture_wait is not None:
                        process.wait = fixture_wait
        if process is None:
            raise ValueError('没有启动测试进程')
        remaining = group_members(process.pid)
        record.update(group_members_after_observation=remaining, leader_exit_after_observation=process.poll(),
                      query_count=queries, injected=injected)
        if remaining or process.returncode is None:
            raise AssertionError('观察返回或抛异常后仍有测试进程，磁盘检查不得继续')
        if failed_query is not None or stale_query or cleanup_interrupted or initial_interrupted:
            if not injected:
                raise AssertionError('没有进入指定的实际进程边界')
        if failed_query is None:
            result = record['observation']
            if result['process_group_empty'] is not True or result['process_group_members']:
                raise AssertionError('已回收的真实进程组没有确认为空')
            if initial_interrupted:
                if result['returncode'] != 'interrupted' or result['timed_out']:
                    raise AssertionError('初始等待收到停止请求后没有结束等待并进入清理')
            elif mode.startswith('leader-exit'):
                if result['returncode'] != 0 or result['timed_out']:
                    raise AssertionError('leader 的自然退出结果没有保留')
                if 'ignore-term' in mode and 'SIGKILL' not in result['cleanup_signals']:
                    raise AssertionError('忽略 SIGTERM 的真实子进程没有完成强制停止')
            elif result['returncode'] != 'timeout' or result['timed_out'] is not True:
                raise AssertionError('超时结果没有保留')
        elif 'observation_error' not in record and not record['observation'].get('errors'):
            raise AssertionError('ps 查询失败没有记录')
        if (cleanup_interrupted or initial_interrupted) and not record['observation'].get('errors'):
            raise AssertionError('观察器收到的停止请求没有记录')
        record['passed'] = True
    except Exception as error:
        record['error'] = {'type': type(error).__name__, 'message': str(error)}
    finally:
        if process is not None:
            try:
                REAL_KILLPG(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
                deadline = time.monotonic() + 3
                remaining = group_members(process.pid)
                while remaining and time.monotonic() < deadline:
                    time.sleep(0.05)
                    remaining = group_members(process.pid)
                record['final_process_group_members'] = remaining
                if remaining:
                    record['passed'] = False
            except Exception as error:
                record['passed'] = False
                record['cleanup_error'] = {'type': type(error).__name__, 'message': str(error)}
        record['elapsed_seconds'] = time.monotonic() - started
        (directory / 'result.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    return record


def output_cases(output):
    source = output / 'preflight-core.json'
    source.write_text(json.dumps({'configuration': {'guest_layout': 'sparse', 'workload': 'core', 'cpus': 1,
                      'execution': 'direct', 'direct': {'memory': 'flat'}, 'data_tbi': True,
                      'guest_page_bytes': 16384, 'mapping': 'pte', 'code_memory': 'dual'},
                      'runs': [{'validated': True, 'results': [{'config': 'translation_guest_layout', 'guest_end': 1 << 39}]}],
                      'sha256': {}}))
    common = Path(REAL_CHECK_OUTPUT(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    main = common.resolve().parent
    checks = []

    class OutputAccepted(Exception):
        pass

    def stop_before_writing(path, *args, **kwargs):
        raise OutputAccepted(str(path))

    cases = [('default-local', ROOT / 'work/native-linux-nce/plain-process-preflight', False, True),
             ('default-external-rejected', main / 'work/native-linux-nce/plain-process-preflight', False, False),
             ('plain-external', main / 'work/native-linux-nce/plain-process-preflight', True, True),
             ('plain-outside-rejected', main.parent / 'plain-process-preflight', True, False)]
    for name, target, plain, accepted in cases:
        argv = ['probe_stock_init.py', str(source), '--runtime', str(output), '--desktop', str(output),
                '--root-metadata', str(output), '--system-image', str(source), '--sdk-ramdisk', str(source),
                '--lifecycle-only', '--output', str(target)]
        if plain:
            argv.append('--sdk-plain-data')
        reached = False
        with patch.object(sys, 'argv', argv), patch.object(Path, 'mkdir', stop_before_writing):
            try:
                probe.main()
            except OutputAccepted:
                reached = True
            except SystemExit as error:
                if error.code != 2:
                    raise
        checks.append({'name': name, 'passed': reached == accepted})
    checks.extend([{'name': 'local-manifest-relative', 'passed': probe._manifest_path(ROOT / 'scripts/probe_stock_init.py') == 'scripts/probe_stock_init.py'},
                   {'name': 'external-manifest-absolute', 'passed': probe._manifest_path(output / 'preflight-core.json') == str(output / 'preflight-core.json')}])
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    common = Path(REAL_CHECK_OUTPUT(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    if not output.is_relative_to(common.resolve().parent / 'work/native-linux-nce'):
        parser.error('输出需要位于 MAIN/work/native-linux-nce')
    output.mkdir(parents=True, exist_ok=False)
    sources = [Path(__file__).resolve(), Path(probe.__file__).resolve(), Path(process_observer.__file__).resolve()]
    before = {str(path): sha(path) for path in sources}
    checks = [process_case(output, 'timeout-parent-child', 'timeout'),
              process_case(output, 'leader-exit-child-remains', 'leader-exit-ignore-term'),
              process_case(output, 'initial-ps-failure', 'timeout', failed_query=1),
              process_case(output, 'cleanup-ps-failure', 'timeout-ignore-term', failed_query=2),
              process_case(output, 'signal-exit-race', 'leader-exit', stale_query=True),
              process_case(output, 'signal-during-cleanup', 'timeout-ignore-term', cleanup_interrupted=True),
              process_case(output, 'signal-during-initial-wait', 'timeout', initial_interrupted=True)]
    checks += output_cases(output)
    unchanged = all(sha(Path(path)) == value for path, value in before.items())
    result = {'passed': unchanged and all(check['passed'] for check in checks), 'source_sha256': before,
              'source_unchanged': unchanged, 'checks': checks,
              'scope': '真实宿主父子进程、ps 查询失败、发送信号竞态与观察器初始等待及清理期间收到 SIGTERM；输出仅验证参数检查；没有执行客体或读取系统镜像。'}
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': len(checks), 'report': str(output / 'result.json')}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
