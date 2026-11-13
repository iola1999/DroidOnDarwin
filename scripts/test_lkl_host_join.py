#!/usr/bin/env python3
"""检查实际 LKL host 线程 join 的正常返回与自 join 失败，覆盖 CLI/GCD。"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys

sys.dont_write_bytecode = True
import process_observer

ROOT = Path(__file__).resolve().parents[1]
EXP = ROOT / 'experiments/native-linux'
HOST = EXP / 'lkl-host.c'
FIXED_JOIN = '''static int thread_join(lkl_thread_t t)
{
    REQUIRE(pthread_join((pthread_t)t, NULL) == 0);
    return 0;
}'''
CONTROL_JOIN = '''static int thread_join(lkl_thread_t t)
{
    return pthread_join((pthread_t)t, NULL) ? -1 : 0;
}'''
HOST_FAILURE = 'LKL host failure: pthread_join((pthread_t)t, NULL) == 0\n'
FIXTURE = r'''
#include "lkl-host-ops.h"
#include <assert.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

extern struct lkl_host_operations nce_lkl_host;

struct request {
    const char *entry;
    const char *mode;
    dispatch_semaphore_t done;
};

static void worker(void *opaque)
{
    assert(!pthread_main_np());
    *(unsigned long *)opaque = 37;
}

static void self_worker(void *opaque)
{
    struct request *request = opaque;
    assert(!pthread_main_np());
    lkl_thread_t self = nce_lkl_host.thread_self();
    assert(self == (lkl_thread_t)(uintptr_t)pthread_self());
    int error = pthread_join(pthread_self(), NULL);
    printf("{\"test\":\"pthread_self_join\",\"entry\":\"%s\",\"error\":%d,"
           "\"expected_EDEADLK\":%d,\"main_thread\":false}\n",
           request->entry, error, EDEADLK);
    fflush(stdout);
    assert(error == EDEADLK);
    int result = nce_lkl_host.thread_join(self);
    printf("{\"test\":\"lkl_host_join_continued\",\"entry\":\"%s\",\"join_result\":%d}\n",
           request->entry, result);
    assert(result == -1);
    dispatch_semaphore_signal(request->done);
}

static void run(void *opaque)
{
    struct request *request = opaque;
    int is_main = pthread_main_np();
    assert(is_main == !strcmp(request->entry, "cli"));
    if (!strcmp(request->mode, "normal")) {
        unsigned long value = 0;
        lkl_thread_t thread = nce_lkl_host.thread_create(worker, &value);
        assert(thread);
        int result = nce_lkl_host.thread_join(thread);
        assert(result == 0 && value == 37);
        printf("{\"test\":\"lkl_host_join_normal\",\"entry\":\"%s\",\"join_result\":%d,"
               "\"worker_value\":%lu,\"main_thread\":%s}\n",
               request->entry, result, value, is_main ? "true" : "false");
    } else {
        assert(!strcmp(request->mode, "self"));
        printf("{\"test\":\"entry_ready\",\"entry\":\"%s\","
               "\"main_thread\":%s}\n",
               request->entry, is_main ? "true" : "false");
        fflush(stdout);
        struct request self_request = {request->entry, request->mode, dispatch_semaphore_create(0)};
        assert(self_request.done);
        lkl_thread_t thread = nce_lkl_host.thread_create(self_worker, &self_request);
        assert(thread);
        /* Keep both self joins before the caller's join. */
        assert(!dispatch_semaphore_wait(self_request.done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
        int result = nce_lkl_host.thread_join(thread);
        assert(result == 0);
        dispatch_release(self_request.done);
        printf("{\"test\":\"self_worker_joined\",\"entry\":\"%s\",\"join_result\":%d}\n",
               request->entry, result);
    }
    if (request->done)
        dispatch_semaphore_signal(request->done);
}

int main(int argc, char **argv)
{
    struct rlimit core = {0, 0};
    assert(!setrlimit(RLIMIT_CORE, &core));
    assert(argc == 3);
    struct request request = {argv[1], argv[2], NULL};
    if (!strcmp(request.entry, "gcd")) {
        request.done = dispatch_semaphore_create(0);
        assert(request.done);
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &request, run);
        assert(!dispatch_semaphore_wait(request.done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
        dispatch_release(request.done);
    } else {
        assert(!strcmp(request.entry, "cli"));
        run(&request);
    }
    printf("{\"test\":\"fixture_complete\",\"entry\":\"%s\",\"mode\":\"%s\"}\n",
           request.entry, request.mode);
    return 0;
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def metadata(path):
    return {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)}


def build(directory, compiler, header, host, fixture):
    directory.mkdir()
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(header.parent), '-I', str(EXP)]
    commands = [
        [compiler, *flags, '-MMD', '-MF', str(directory / 'host.d'), '-c', str(host), '-o', str(directory / 'host.o')],
        [compiler, *flags, '-MMD', '-MF', str(directory / 'fixture.d'), '-c', str(fixture), '-o', str(directory / 'fixture.o')],
        [compiler, str(directory / 'host.o'), str(directory / 'fixture.o'), '-o', str(directory / 'fixture')],
    ]
    with (directory / 'build.log').open('w') as log:
        for command in commands:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=30)
    symbols = subprocess.check_output(['nm', '-g', str(directory / 'fixture')], text=True, timeout=10)
    (directory / 'symbols.log').write_text(symbols)
    assert any(line.split()[-1:] == ['_nce_lkl_host'] for line in symbols.splitlines())
    dependencies = {name: metadata(directory / name) for name in ('host.d', 'fixture.d')}
    assert str(header) in (directory / 'host.d').read_text()
    assert str(EXP / 'layout.h') in (directory / 'host.d').read_text()
    return {'commands': commands, 'dependencies': dependencies, 'binary': metadata(directory / 'fixture'),
            'host_source': metadata(host)}


def run_case(output, binary, version, entry, mode):
    directory = output / f'{version}-{entry}-{mode}'
    directory.mkdir()
    command = [str(binary), entry, mode]
    record = {'version': version, 'entry': entry, 'mode': mode, 'command': command, 'passed': False}
    try:
        stdout_path, stderr_path = directory / 'stdout.log', directory / 'stderr.log'
        with stdout_path.open('w') as stdout, stderr_path.open('w') as stderr:
            observed = process_observer.observe(command, os.environ.copy(), stdout, stderr, 10)
        record['observation'] = observed
        record['stdout'], record['stderr'] = metadata(stdout_path), metadata(stderr_path)
        try:
            os.waitpid(observed['pid'], os.WNOHANG)
        except ChildProcessError:
            record['leader_reaped_by_observer'] = True
        else:
            raise AssertionError('共享观察器返回前没有回收 leader')
        ps_rows = subprocess.check_output(['ps', '-axo', 'pid=,pgid='], text=True, timeout=10)
        remaining = [int(fields[0]) for line in ps_rows.splitlines()
                     if len(fields := line.split()) == 2 and int(fields[1]) == observed['pid']]
        record['process_group_members_after_observation'] = remaining
        assert remaining == []
        assert observed['process_group_empty'] is True and observed['process_group_members'] == []
        assert not observed['timed_out'] and not observed['errors'] and not observed['cleanup_signals']
        assert not observed['output_limit_exceeded']
        rows = [json.loads(line) for line in stdout_path.read_text().splitlines()]
        stderr_text = stderr_path.read_text()
        record['rows'] = rows
        with_rejection = version == 'fixed' and mode == 'self'
        expected_exit = -signal.SIGABRT if with_rejection else 0
        assert observed['returncode'] == observed['actual_exit'] == expected_exit
        complete = {'test': 'fixture_complete', 'entry': entry, 'mode': mode}
        if mode == 'normal':
            assert rows == [{'test': 'lkl_host_join_normal', 'entry': entry, 'join_result': 0,
                             'worker_value': 37, 'main_thread': entry == 'cli'}, complete]
            assert stderr_text == ''
        else:
            ready = {'test': 'entry_ready', 'entry': entry, 'main_thread': entry == 'cli'}
            assert rows[0] == ready
            first = rows[1]
            assert set(first) == {'test', 'entry', 'error', 'expected_EDEADLK', 'main_thread'}
            assert first['test'] == 'pthread_self_join' and first['entry'] == entry
            assert type(first['error']) is int and type(first['expected_EDEADLK']) is int
            assert first['error'] == first['expected_EDEADLK'] == errno.EDEADLK
            assert first['main_thread'] is False
            if with_rejection:
                assert rows == [ready, first] and stderr_text == HOST_FAILURE
            else:
                assert rows == [ready, first,
                                {'test': 'lkl_host_join_continued', 'entry': entry, 'join_result': -1},
                                {'test': 'self_worker_joined', 'entry': entry, 'join_result': 0}, complete]
                assert stderr_text == ''
        record['passed'] = True
    except Exception as error:
        record['error'] = {'type': type(error).__name__, 'message': str(error)}
    (directory / 'result.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--host-ops', type=Path, default=ROOT / 'work/native-linux-nce/lkl-translation-kernel-bpf-jit/lkl-host-ops.h')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('本回归需要实际 Darwin pthread 与 GCD')
    common = Path(subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=ROOT, text=True, timeout=10).strip())
    common = common if common.is_absolute() else ROOT / common
    main_root = common.resolve().parent
    output = args.output.resolve()
    if not (output.is_relative_to(main_root / 'work/sdk-plain-validation-v1/lkl-host-join-v1')
            or output.is_relative_to(ROOT / 'work/native-linux-nce')):
        parser.error('输出需要位于本轮验证目录或 F/work/native-linux-nce')
    header = args.host_ops.resolve()
    if not header.is_file() or header.name != 'lkl-host-ops.h':
        parser.error('--host-ops 需要实际 lkl-host-ops.h')
    output.mkdir(parents=True, exist_ok=False)
    sources = (Path(__file__).resolve(), HOST, EXP / 'layout.h', header, Path(process_observer.__file__).resolve())
    before = {str(path): sha(path) for path in sources}
    result = {'test': 'lkl_host_thread_join', 'passed': False, 'host': platform.platform(),
              'source_sha256_before': before, 'builds': {}, 'checks': [],
              'scope': 'CLI/GCD 分别调用实际 nce_lkl_host.thread_create/thread_join；joinable worker 的真实自 join 返回 EDEADLK，控制版继续并回收 worker，修复版 SIGABRT。'}
    try:
        fixed_source = HOST.read_text()
        if fixed_source.count(FIXED_JOIN) != 1:
            raise ValueError('实际 thread_join 源码与修复契约不同')
        control = output / 'control-host.c'
        control.write_text(fixed_source.replace(FIXED_JOIN, CONTROL_JOIN, 1))
        fixture = output / 'fixture.c'
        fixture.write_text(FIXTURE)
        compiler = shutil.which('clang')
        if not compiler:
            raise RuntimeError('缺少 clang')
        result['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True, timeout=10).splitlines()[0]
        result['fixture_source'] = metadata(fixture)
        result['control_join_source'] = CONTROL_JOIN
        for version, source in (('fixed', HOST), ('control', control)):
            result['builds'][version] = build(output / version, compiler, header, source, fixture)
        for entry in ('cli', 'gcd'):
            result['checks'].append(run_case(output, output / 'fixed/fixture', 'fixed', entry, 'normal'))
            result['checks'].append(run_case(output, output / 'control/fixture', 'control', entry, 'self'))
            result['checks'].append(run_case(output, output / 'fixed/fixture', 'fixed', entry, 'self'))
    except (OSError, ValueError, AssertionError, RuntimeError, subprocess.SubprocessError) as error:
        result['error'] = {'type': type(error).__name__, 'message': str(error)}
    after = {str(path): sha(path) for path in sources}
    result['source_sha256_after'] = after
    result['source_unchanged'] = before == after
    result['passed'] = ('error' not in result and result['source_unchanged'] and
                        len(result['checks']) == 6 and all(check['passed'] for check in result['checks']))
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': result['passed'], 'checks': len(result['checks']), 'report': str(output / 'result.json')}, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
