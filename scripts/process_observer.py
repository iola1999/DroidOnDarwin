"""观察宿主进程退出、中断与进程组回收。"""
import os
import signal
import subprocess
import time

OBSERVATION_MAX_BYTES = 32 << 20


def observe(command, env, stdout, stderr, timeout):
    errors, signals = [], []
    interruption = None

    def record_error(phase, error):
        errors.append({'phase': phase, 'type': type(error).__name__, 'errno': getattr(error, 'errno', None)})

    def members(group, phase):
        try:
            rows = subprocess.check_output(['ps', '-axo', 'pid=,pgid='], text=True, timeout=10)
            return [int(fields[0]) for row in rows.splitlines()
                    if len(fields := row.split()) == 2 and int(fields[1]) == group]
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            record_error(phase, error)
            return None

    def interrupted(signum, _frame):
        nonlocal interruption
        interruption = signum
        record_error('interrupt', InterruptedError(signum, 'stock init observer interrupted'))

    def log_sizes():
        return {name: os.fstat(stream.fileno()).st_size for name, stream in (('stdout', stdout), ('stderr', stderr))}

    process = subprocess.Popen(command, env=env, stdout=stdout, stderr=stderr, start_new_session=True)
    group = process.pid
    handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
    for sig in handlers:
        signal.signal(sig, interrupted)
    returncode, timed_out, remaining = 'error', False, None
    observed_log_bytes = {}
    output_limit_exceeded = False
    try:
        try:
            deadline = time.monotonic() + timeout
            while interruption is None:
                observed_log_bytes = log_sizes()
                if any(size > OBSERVATION_MAX_BYTES for size in observed_log_bytes.values()):
                    output_limit_exceeded = True
                    break
                try:
                    returncode = process.wait(timeout=min(0.1, max(0, deadline - time.monotonic())))
                    break
                except subprocess.TimeoutExpired:
                    if time.monotonic() >= deadline:
                        returncode, timed_out = 'timeout', True
                        break
            if interruption is not None:
                returncode = 'interrupted'
        except BaseException as error:
            record_error('wait', error)
        remaining = members(group, 'after_wait')
        for sig, label in ((signal.SIGTERM, 'SIGTERM'), (signal.SIGKILL, 'SIGKILL')):
            if remaining == []:
                break
            try:
                os.killpg(group, sig)
                signals.append(label)
            except ProcessLookupError:
                pass
            except OSError as error:
                record_error('signal_' + label, error)
            deadline = time.monotonic() + 5
            while remaining != [] and time.monotonic() < deadline:
                try:
                    if process.poll() is None:
                        process.wait(timeout=0.1)
                    else:
                        time.sleep(0.1)
                except subprocess.TimeoutExpired:
                    pass
                except BaseException as error:
                    record_error('cleanup_wait', error)
                    break
                remaining = members(group, 'after_' + label)
            remaining = members(group, 'signal_end_' + label)
        if process.poll() is None:
            try:
                process.wait(timeout=5)
            except BaseException as error:
                record_error('final_wait', error)
        remaining = members(group, 'final')
        try:
            observed_log_bytes = log_sizes()
            output_limit_exceeded |= any(size > OBSERVATION_MAX_BYTES for size in observed_log_bytes.values())
        except (OSError, ValueError) as error:
            record_error('final_log_sizes', error)
    finally:
        for sig, previous in handlers.items():
            signal.signal(sig, previous)
    if output_limit_exceeded:
        returncode = 'log_limit'
    return {'pid': group, 'returncode': returncode, 'actual_exit': process.returncode,
            'timed_out': timed_out, 'timeout_seconds': timeout, 'cleanup_signals': signals,
            'output_limit_exceeded': output_limit_exceeded, 'log_limit_bytes': OBSERVATION_MAX_BYTES,
            'observed_log_bytes': observed_log_bytes,
            'errors': errors, 'process_group_members': remaining,
            'process_group_empty': remaining == [] and process.returncode is not None}
