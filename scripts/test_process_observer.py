#!/usr/bin/env python3
"""用真实宿主进程检查退出、SIGTERM 中断和父子进程组回收。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
from process_observer import OBSERVATION_MAX_BYTES, observe

FIXTURE = r'''
import json, os, signal, sys, time
from pathlib import Path
mode, ready_path = sys.argv[1:]
reader, writer = os.pipe()
child = os.fork()
if child == 0:
    os.close(reader)
    if mode == 'ignore-term':
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    os.write(writer, b'r')
    os.close(writer)
    time.sleep(60)
    os._exit(0)
os.close(writer)
assert os.read(reader, 1) == b'r'
os.close(reader)
Path(ready_path).write_text(json.dumps({'parent': os.getpid(), 'child': child, 'group': os.getpgrp()}))
time.sleep(60)
'''


def group_members(group):
    rows = subprocess.check_output(['ps', '-axo', 'pid=,pgid='], text=True, timeout=10)
    return [int(fields[0]) for row in rows.splitlines()
            if len(fields := row.split()) == 2 and int(fields[1]) == group]


def process_case(output, name, mode):
    directory = output / name
    directory.mkdir()
    ready = directory / 'ready.json'
    command = ([sys.executable, '-c', 'print("normal exit")'] if mode is None else
               [sys.executable, '-c', FIXTURE, mode, str(ready)])
    record = {'name': name, 'command': command, 'passed': False}
    handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
    cancel = threading.Event()

    def send_interrupt():
        deadline = time.monotonic() + 3
        try:
            while not cancel.is_set() and time.monotonic() < deadline:
                try:
                    fixture = json.loads(ready.read_text())
                except (FileNotFoundError, json.JSONDecodeError):
                    cancel.wait(0.01)
                    continue
                if signal.getsignal(signal.SIGTERM) == handlers[signal.SIGTERM]:
                    cancel.wait(0.01)
                    continue
                if fixture['parent'] != fixture['group'] or not group_members(fixture['group']):
                    raise AssertionError('实际宿主父子进程组尚未准备完成')
                record['fixture'] = fixture
                os.kill(os.getpid(), signal.SIGTERM)
                record['sent_signal'] = 'SIGTERM'
                return
            if not cancel.is_set():
                raise TimeoutError('实际宿主父子进程没有完成准备')
        except Exception as error:
            record['sender_error'] = {'type': type(error).__name__, 'message': str(error)}

    sender = threading.Thread(target=send_interrupt) if mode is not None else None
    started = time.monotonic()
    try:
        if sender:
            sender.start()
        with (directory / 'stdout.log').open('w') as stdout, (directory / 'stderr.log').open('w') as stderr:
            result = observe(command, os.environ.copy(), stdout, stderr, 10)
            record['observation'] = result
        cancel.set()
        if sender:
            sender.join(timeout=3)
            if sender.is_alive() or 'sender_error' in record or record.get('sent_signal') != 'SIGTERM':
                raise AssertionError('实际 SIGTERM 发送没有完成')
        record['process_group_members_after_observation'] = group_members(result['pid'])
        if (result['process_group_empty'] is not True or result['process_group_members']
                or record['process_group_members_after_observation'] or result['actual_exit'] is None):
            raise AssertionError('观察结束后仍有宿主父子进程')
        if any(signal.getsignal(sig) != previous for sig, previous in handlers.items()):
            raise AssertionError('观察结束后没有恢复原 signal handler')
        if result['timed_out'] or result['output_limit_exceeded'] or result['log_limit_bytes'] != OBSERVATION_MAX_BYTES:
            raise AssertionError('小型进程检查的退出或日志上限结果异常')
        if mode is None:
            if result['returncode'] != 0 or result['actual_exit'] != 0 or result['errors'] or result['cleanup_signals']:
                raise AssertionError('自然退出的真实状态没有保留')
        else:
            if (result['returncode'] != 'interrupted' or result['actual_exit'] != -signal.SIGTERM
                    or [error['phase'] for error in result['errors']] != ['interrupt']
                    or result['errors'][0]['errno'] != signal.SIGTERM
                    or result['cleanup_signals'][0] != 'SIGTERM'):
                raise AssertionError('SIGTERM 中断或宿主实际退出状态没有保留')
            if mode == 'ignore-term' and result['cleanup_signals'] != ['SIGTERM', 'SIGKILL']:
                raise AssertionError('忽略 SIGTERM 的实际子进程没有完成强制停止')
        record['passed'] = True
    except Exception as error:
        record['error'] = {'type': type(error).__name__, 'message': str(error)}
    finally:
        cancel.set()
        if sender:
            sender.join(timeout=3)
        fixture = record.get('fixture', record.get('observation', {}))
        group = fixture.get('group', fixture.get('pid'))
        if group:
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
            deadline = time.monotonic() + 3
            remaining = group_members(group)
            while remaining and time.monotonic() < deadline:
                time.sleep(0.05)
                remaining = group_members(group)
            record['final_process_group_members'] = remaining
            if remaining:
                record['passed'] = False
        record['elapsed_seconds'] = time.monotonic() - started
        (directory / 'result.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cases = [('natural-exit', None), ('observer-sigterm', 'default'), ('observer-sigterm-ignore-term', 'ignore-term')]
    results = [process_case(output, name, mode) for name, mode in cases]
    paths = (Path(__file__).resolve(), Path(observe.__code__.co_filename).resolve())
    summary = {'scope': '真实 macOS 宿主进程退出、SIGTERM 中断及父子进程组回收',
               'platform': platform.platform(), 'python': sys.version,
               'source_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths},
               'results': results, 'passed': all(result['passed'] for result in results)}
    (output / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': summary['passed'], 'cases': len(results), 'output': str(output)}, ensure_ascii=False))
    if not summary['passed']:
        raise SystemExit('共享宿主进程观察检查失败')


if __name__ == '__main__':
    main()
