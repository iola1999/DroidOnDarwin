#!/usr/bin/env python3
"""验证观察日志限额与超界后的宿主进程回收，仅使用小文件和宿主进程。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
import probe_stock_init as probe
import process_observer

ROOT = Path(__file__).resolve().parents[1]
REAL_POPEN = subprocess.Popen
REAL_CHECK_OUTPUT = subprocess.check_output
REAL_KILLPG = os.killpg
REAL_PATH_OPEN = Path.open
SMALL_LIMIT = 4096
FIXTURE_MAX_BYTES = 16384
FIXTURE = r'''
import json, os, signal, sys, time
from pathlib import Path
mode, ready_path, go_path = sys.argv[1:]
signal.signal(signal.SIGTERM, signal.SIG_IGN)
reader, writer = os.pipe()
child = os.fork()
if child == 0:
    os.close(reader)
    os.write(writer, b'r')
    os.close(writer)
    while not Path(go_path).exists():
        time.sleep(0.01)
    time.sleep(0.15)
    if mode == 'boundary':
        os.write(1, b'x' * 4096)
        os.write(2, b'x' * 4096)
        os._exit(0)
    descriptor = 2 if mode == 'stderr' else 1
    for _ in range(32):
        os.write(descriptor, b'x' * 512)
        time.sleep(0.02)
    time.sleep(60)
    os._exit(0)
os.close(writer)
assert os.read(reader, 1) == b'r'
os.close(reader)
Path(ready_path).write_text(json.dumps({'parent': os.getpid(), 'child': child, 'group': os.getpgrp()}))
if mode == 'leader-exit':
    os._exit(0)
if mode == 'boundary':
    _, status = os.waitpid(child, 0)
    os._exit(os.waitstatus_to_exitcode(status))
time.sleep(60)
'''


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def group_members(group):
    rows = REAL_CHECK_OUTPUT(['ps', '-axo', 'pid=,pgid='], text=True, timeout=10)
    return [int(fields[0]) for row in rows.splitlines()
            if len(fields := row.split()) == 2 and int(fields[1]) == group]


class RecordedReads:
    def __init__(self, source, calls):
        self.source, self.calls = source, calls

    def __enter__(self):
        self.source.__enter__()
        return self

    def __exit__(self, *args):
        return self.source.__exit__(*args)

    def read(self, size=-1):
        self.calls.append(('read', size))
        return self.source.read(size)

    def readline(self, size=-1):
        self.calls.append(('readline', size))
        return self.source.readline(size)


class ObservationLimits(unittest.TestCase):
    output = None

    def setUp(self):
        self.directory = self.output / self._testMethodName
        self.directory.mkdir()
        limits = patch.multiple(probe, _OBSERVATION_MAX_BYTES=SMALL_LIMIT,
                                _OBSERVATION_MAX_LINE_BYTES=128, _OBSERVATION_MAX_ROWS=2)
        limits.start()
        self.addCleanup(limits.stop)
        process_limit = patch.object(process_observer, 'OBSERVATION_MAX_BYTES', SMALL_LIMIT)
        process_limit.start()
        self.addCleanup(process_limit.stop)

    def assert_bounded_reads(self, calls, byte_limit, line_limit):
        self.assertTrue(calls)
        for method, size in calls:
            self.assertGreater(size, 0)
            self.assertLessEqual(size, (byte_limit if method == 'read' else line_limit) + 1)

    def test_exact_byte_boundary(self):
        rows = [{'value': '中'}, {'value': 1}]
        data = b''.join(json.dumps(row, ensure_ascii=False, separators=(',', ':')).encode() + b'\n' for row in rows)
        path = self.directory / 'boundary.log'
        path.write_bytes(data)
        with patch.object(probe, '_OBSERVATION_MAX_BYTES', len(data)):
            self.assertEqual(probe.read_observations(path), rows)
            self.assertEqual(probe._read_log_text(path), data.decode())

    def test_sparse_overflow_rejected_before_open(self):
        path = self.directory / 'sparse.log'
        with path.open('wb') as stream:
            stream.truncate(SMALL_LIMIT + 1)
        self.assertEqual(path.stat().st_size, SMALL_LIMIT + 1)
        for reader in (probe.read_observations, probe._read_log_text):
            with self.subTest(reader=reader.__name__):
                with patch.object(Path, 'open', side_effect=AssertionError('超界文件不得开始读取')) as opened:
                    with self.assertRaises(ValueError):
                        reader(path)
                    opened.assert_not_called()

    def test_total_bytes_enforced_after_stat(self):
        for reader in (probe.read_observations, probe._read_log_text):
            with self.subTest(reader=reader.__name__):
                path = self.directory / (reader.__name__ + '.log')
                path.write_bytes(b'{}\n')
                grew = False
                calls = []

                def growing_open(candidate, *args, **kwargs):
                    nonlocal grew
                    if candidate == path and not grew:
                        grew = True
                        with REAL_PATH_OPEN(path, 'ab') as stream:
                            stream.write(b'{}\n')
                    return RecordedReads(REAL_PATH_OPEN(candidate, *args, **kwargs), calls)

                with patch.object(probe, '_OBSERVATION_MAX_BYTES', 3), patch.object(Path, 'open', growing_open):
                    with self.assertRaises(ValueError):
                        reader(path)
                self.assertTrue(grew)
                self.assertEqual(path.stat().st_size, 6)
                self.assert_bounded_reads(calls, 3, 128)

    def test_exact_line_boundary(self):
        for newline in (b'\n', b''):
            with self.subTest(newline=bool(newline)):
                prefix, suffix = b'{"value":"', b'"}' + newline
                value = b'x' * (32 - len(prefix) - len(suffix))
                data = prefix + value + suffix
                self.assertEqual(len(data), 32)
                path = self.directory / ('newline.log' if newline else 'eof.log')
                path.write_bytes(data)
                with patch.object(probe, '_OBSERVATION_MAX_LINE_BYTES', 32):
                    self.assertEqual(probe.read_observations(path), [{'value': value.decode()}])

    def test_long_json_and_diagnostic_lines(self):
        for json_line in (True, False):
            for newline in (b'\n', b''):
                with self.subTest(json_line=json_line, newline=bool(newline)):
                    if json_line:
                        prefix, suffix = b'{"value":"', b'"}' + newline
                        data = prefix + b'x' * (33 - len(prefix) - len(suffix)) + suffix
                    else:
                        data = b'x' * (33 - len(newline)) + newline
                    path = self.directory / f'line-{int(json_line)}-{int(bool(newline))}.log'
                    path.write_bytes(data)
                    calls = []

                    def recording_open(candidate, *args, **kwargs):
                        return RecordedReads(REAL_PATH_OPEN(candidate, *args, **kwargs), calls)

                    with patch.object(probe, '_OBSERVATION_MAX_LINE_BYTES', 32), patch.object(Path, 'open', recording_open):
                        with self.assertRaises(ValueError):
                            probe.read_observations(path)
                    self.assert_bounded_reads(calls, SMALL_LIMIT, 32)

    def test_json_record_boundary_and_overflow(self):
        path = self.directory / 'rows.log'
        path.write_bytes(b'diagnostic\n{}\n\n{}\n')
        self.assertEqual(probe.read_observations(path), [{}, {}])
        with path.open('ab') as stream:
            stream.write(b'{}\n')
        with self.assertRaises(ValueError):
            probe.read_observations(path)

    def test_invalid_json_rejected(self):
        for index, data in enumerate((b'{broken}\n', b'{}\n{"value":\n')):
            with self.subTest(index=index):
                path = self.directory / f'invalid-{index}.log'
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    probe.read_observations(path)

    def process_case(self, mode):
        ready = self.directory / 'ready.json'
        go = self.directory / 'go'
        stdout_path = self.directory / 'stdout.log'
        stderr_path = self.directory / 'stderr.log'
        command = [sys.executable, '-c', FIXTURE, mode, str(ready), str(go)]
        process = None
        record = {'mode': mode, 'passed': False}

        def launch(*args, **kwargs):
            nonlocal process
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
                    raise AssertionError('测试进程组与实际 leader 不一致')
                record['fixture'] = identity
                go.touch()
                return created
            raise TimeoutError('宿主父子进程没有完成准备')

        try:
            with stdout_path.open('wb') as stdout, stderr_path.open('wb') as stderr:
                with patch.object(process_observer.subprocess, 'Popen', launch):
                    observed = probe._observe(command, os.environ.copy(), stdout, stderr, 3)
            record['observation'] = observed
            self.assertIsNotNone(process)
            record['leader_exit_after_observation'] = process.returncode
            # 测试自身的 poll/wait 会回收 leader，先单独核对 _observe 已完成 wait。
            with self.assertRaises(ChildProcessError):
                os.waitpid(process.pid, os.WNOHANG)
            record['leader_reaped_by_observer'] = True
            remaining = group_members(process.pid)
            record['group_members_after_observation'] = remaining
            sizes = {'stdout': stdout_path.stat().st_size, 'stderr': stderr_path.stat().st_size}
            record['actual_log_bytes'] = sizes
            self.assertEqual(remaining, [])
            self.assertIs(observed['process_group_empty'], True)
            self.assertEqual(observed['process_group_members'], [])
            self.assertEqual(observed['pid'], process.pid)
            self.assertEqual(observed['actual_exit'], process.returncode)
            self.assertEqual(observed['log_limit_bytes'], SMALL_LIMIT)
            self.assertEqual(observed['observed_log_bytes'], sizes)
            self.assertIs(observed['timed_out'], False)
            if mode == 'boundary':
                self.assertEqual(sizes, {'stdout': SMALL_LIMIT, 'stderr': SMALL_LIMIT})
                self.assertIs(observed['output_limit_exceeded'], False)
                self.assertEqual(observed['returncode'], 0)
                self.assertEqual(observed['actual_exit'], 0)
            else:
                stream_name = 'stderr' if mode == 'stderr' else 'stdout'
                other = 'stdout' if stream_name == 'stderr' else 'stderr'
                self.assertGreater(sizes[stream_name], SMALL_LIMIT)
                self.assertLessEqual(sizes[stream_name], FIXTURE_MAX_BYTES)
                self.assertEqual(sizes[other], 0)
                self.assertIs(observed['output_limit_exceeded'], True)
                self.assertEqual(observed['returncode'], 'log_limit')
                self.assertIn('SIGTERM', observed['cleanup_signals'])
                self.assertIn('SIGKILL', observed['cleanup_signals'])
                self.assertEqual(observed['actual_exit'], 0 if mode == 'leader-exit' else -signal.SIGKILL)
            record['passed'] = True
        except Exception as error:
            record['error'] = {'type': type(error).__name__, 'message': str(error)}
            raise
        finally:
            try:
                if process is not None:
                    try:
                        REAL_KILLPG(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=5)
                    deadline = time.monotonic() + 3
                    remaining = group_members(process.pid)
                    while remaining and time.monotonic() < deadline:
                        time.sleep(0.05)
                        remaining = group_members(process.pid)
                    record['final_process_group_members'] = remaining
                    if remaining:
                        raise AssertionError('测试进程组清理失败')
            except Exception as error:
                record['passed'] = False
                record['cleanup_error'] = {'type': type(error).__name__, 'message': str(error)}
                raise
            finally:
                (self.directory / 'result.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')

    def test_stdout_growth_stops_parent_and_child(self):
        self.process_case('stdout')

    def test_stderr_growth_stops_parent_and_child(self):
        self.process_case('stderr')

    def test_leader_exit_then_child_growth(self):
        self.process_case('leader-exit')

    def test_exact_stdout_and_stderr_boundary(self):
        self.process_case('boundary')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    common = Path(REAL_CHECK_OUTPUT(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    output = args.output.resolve()
    if not output.is_relative_to(common.resolve().parent / 'work/native-linux-nce'):
        parser.error('输出需要位于 MAIN/work/native-linux-nce')
    output.mkdir(parents=True, exist_ok=False)
    sources = (Path(__file__).resolve(), Path(probe.__file__).resolve(), Path(process_observer.__file__).resolve())
    before = {str(path): sha(path) for path in sources}
    ObservationLimits.output = output
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ObservationLimits))
    unchanged = all(sha(Path(path)) == value for path, value in before.items())
    report = {'passed': result.wasSuccessful() and unchanged, 'tests_run': result.testsRun,
              'failures': [{'test': str(test), 'traceback': trace} for test, trace in result.failures],
              'errors': [{'test': str(test), 'traceback': trace} for test, trace in result.errors],
              'source_sha256': before, 'source_unchanged': unchanged,
              'scope': '小文件的字节、行、JSON 记录限制与实际文件增长；真实宿主父子进程的 stdout/stderr 超限及停止后回收。'}
    (output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'tests': result.testsRun, 'report': str(output / 'result.json')}, ensure_ascii=False))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
