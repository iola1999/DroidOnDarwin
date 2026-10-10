#!/usr/bin/env python3
"""检查显示服务延迟注册、进程退出和等待到期的行为。"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'work/native-linux-nce/display-service-checks'
SOURCE = r'''
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <android/binder_ibinder.h>

struct Failure { int code; };
static int polls, sleeps, released, ready_after, exited_after, launches;
static pid_t fake_fork() { return ++launches; }
static pid_t fake_waitpid(pid_t child, int *status, int) {
    if (exited_after >= 0 && sleeps >= exited_after) {
        *status = 9;
        return child;
    }
    return 0;
}
static int fake_usleep(useconds_t delay) {
    assert(delay == 100000);
    ++sleeps;
    return 0;
}
[[noreturn]] static void fake_exit(int code) { throw Failure{code}; }
AIBinder *AServiceManager_checkService(const char *name) {
    static AIBinder binder;
    if (strcmp(name, "SurfaceFlingerAIDL")) return &binder;
    return polls++ >= ready_after ? &binder : nullptr;
}
void AIBinder_decStrong(AIBinder *) { ++released; }
int __system_property_set(const char *, const char *) { return 0; }

// Compile the real startup function; only its OS/Binder boundaries are replaced.
#define fork fake_fork
#define waitpid fake_waitpid
#define usleep fake_usleep
#define _exit fake_exit
#include "display-service-test.cpp"

static void reset(int ready, int exited) {
    polls = sleeps = released = launches = 0;
    ready_after = ready;
    exited_after = exited;
}
int main() {
    reset(2, -1);
    nce_display_start();
    assert(launches == 3 && polls == 3 && sleeps == 2 && released == 4);
    reset(0, -1);
    nce_display_start();
    assert(polls == 1 && sleeps == 0 && released == 4);
    reset(999, 2);
    try {
        nce_display_start();
        assert(false);
    } catch (Failure f) {
        assert(f.code == 115 && sleeps == 2 && polls == 2 && released == 3);
    }
    reset(999, -1);
    try {
        nce_display_start();
        assert(false);
    } catch (Failure f) {
        assert(f.code == 115 && sleeps == 300 && polls == 300 && released == 3);
    }
    puts("display service readiness: four checks passed");
}
'''


def main():
    (OUT / 'android').mkdir(parents=True, exist_ok=True)
    (OUT / 'sys').mkdir(exist_ok=True)
    (OUT / 'android/binder_ibinder.h').write_text(
        '#pragma once\nstruct AIBinder {};\nvoid AIBinder_decStrong(AIBinder *);\n')
    (OUT / 'android/binder_manager.h').write_text(
        '#include <android/binder_ibinder.h>\nAIBinder *AServiceManager_checkService(const char *);\n')
    (OUT / 'sys/system_properties.h').write_text('int __system_property_set(const char *, const char *);\n')
    (OUT / 'main.cpp').write_text(SOURCE)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    subprocess.run(['xcrun', 'clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-isysroot', sdk, '-I', str(OUT), '-I', str(ROOT / 'experiments/native-linux/userspace'),
                    str(OUT / 'main.cpp'), '-o', str(OUT / 'check')], check=True, timeout=60)
    subprocess.run([OUT / 'check'], check=True, timeout=10)


if __name__ == '__main__':
    main()
