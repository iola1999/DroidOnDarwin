#!/usr/bin/env python3
"""检查生成内核在最后一个宿主任务退出时的 CPU 交接。"""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'work/native-linux-nce'

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define MAX_THREADS 1000000
#define READ_ONCE(x) (x)
#define TIF_HOST_THREAD 1
static struct {
    unsigned shutdown_gate, count, sleepers;
    bool irqs_pending;
    int owner, lock, sem;
} cpu;
static bool locked, host_task, single;
static unsigned scheduled, wakes;
static void lock(int unused) { (void)unused; assert(!locked); locked = true; }
static void unlock(int unused) { (void)unused; assert(locked); locked = false; }
static int self(void) { return 1; }
static bool equal(int a, int b) { return a == b; }
static void up(int unused) { (void)unused; ++wakes; }
static struct {
    void (*mutex_lock)(int), (*mutex_unlock)(int);
    int (*thread_self)(void);
    bool (*thread_equal)(int, int);
    void (*sem_up)(int);
} ops = {lock, unlock, self, equal, up}, *lkl_ops = &ops;
static void lkl_bug(const char *format, const char *name) {
    fprintf(stderr, format, name); abort();
}
static bool irqs_disabled(void) { return false; }
static bool in_interrupt(void) { return false; }
static void run_irqs(void) { assert(!locked); }
static int current_thread_info(void) { return 0; }
static bool test_ti_thread_flag(int ti, int flag) { (void)ti; (void)flag; return host_task; }
static bool single_task_running(void) { return single; }
static void thread_sched_jb(void) { assert(!locked); ++scheduled; }
'''

CHECKS = r'''
static void reset(bool stopping, bool only_host, unsigned depth, unsigned waiters, bool host)
{
    cpu.shutdown_gate = stopping ? MAX_THREADS : 0;
    cpu.count = depth;
    cpu.owner = 1;
    cpu.sleepers = waiters;
    cpu.irqs_pending = false;
    locked = false;
    host_task = host;
    single = only_host;
    scheduled = wakes = 0;
}

int main(void)
{
    /* With IRQ admission closed, the idle task must receive execution before
     * this last host task sleeps. A waiting host caller cannot replace it. */
    for (unsigned waiters = 0; waiters < 2; ++waiters) {
        reset(true, true, 1, waiters, true);
        cpu_put(true);
        if (scheduled != 1 || cpu.count != 1 || cpu.owner != 1 || locked || wakes) {
            fputs("shutdown left the last host task without scheduling idle\n", stderr);
            return 1;
        }
    }
    reset(false, true, 1, 0, true);
    cpu_put(true);
    assert(!scheduled && !cpu.count && !cpu.owner && !locked);
    reset(false, false, 1, 1, true);
    cpu_put(true);
    assert(!scheduled && !cpu.count && !cpu.owner && wakes == 1 && !locked);
    reset(false, false, 1, 0, true);
    cpu_put(false);
    assert(scheduled == 1 && cpu.count == 1 && !locked);
    reset(true, true, 2, 0, true);
    cpu_put(true);
    assert(!scheduled && cpu.count == 1 && cpu.owner == 1 && !locked);
    reset(true, true, 1, 0, false);
    cpu_put(false);
    assert(!scheduled && !cpu.count && !cpu.owner && !locked);
    puts("shutdown handoff and normal ownership boundaries passed");
}
'''


def main():
    source = WORK / 'lkl-translation-smp-kernel/source/arch/lkl/kernel/cpu.c'
    content = source.read_text()
    start = content.index('static void cpu_put(bool idle_handoff)\n{')
    end = content.index('\n}\n', start) + 3
    body = content[start:end]
    condition = 'READ_ONCE(cpu.shutdown_gate) >= MAX_THREADS'
    if body.count(condition) != 1:
        raise RuntimeError('请先构建含停止交接修复的双核内核')
    out = WORK / 'lkl-shutdown-handoff-test'
    out.mkdir(exist_ok=True)
    results = []
    for name, implementation in (('fixed', body), ('negative-control', body.replace(condition, 'false'))):
        path = out / (name + '.c')
        binary = out / name
        path.write_text(PRELUDE + implementation + CHECKS)
        subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)],
                       check=True, timeout=60)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=10)
        results.append({'variant': name, 'returncode': result.returncode, 'stdout': result.stdout,
                        'stderr': result.stderr})
    report = {'source': str(source.relative_to(ROOT)), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'scope': 'CPU ownership handoff boundaries; real Linux service and desktop tests are separate',
              'runs': results}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    if results[0]['returncode'] != 0 or results[1]['returncode'] != 1:
        raise SystemExit('停止交接检查或移除修复后的失败对照异常')
    print(out / 'result.json')


if __name__ == '__main__':
    main()
