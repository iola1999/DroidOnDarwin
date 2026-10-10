#!/usr/bin/env python3
"""检查桌面等待的真实子进程退出、取消回收与就绪事件边界。"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'work/native-linux-nce/desktop-wait-checks'
SOURCE = r'''
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "desktop-session.h"

static bool stop, surface_cancel, stop_after_surface;
static int interruptions, reap_interruptions, commands, surface_calls;
static pid_t wait_child(pid_t child, int *status, int options) {
    if (interruptions || (!options && reap_interruptions)) {
        if (interruptions) --interruptions; else --reap_interruptions;
        errno = EINTR;
        return -1;
    }
    return waitpid(child, status, options);
}
static int access_file(const char *path, int mode) {
    (void)mode;
    if (strcmp(path, "/data/nce/session-stop") == 0 && stop) return 0;
    errno = ENOENT;
    return -1;
}
#define waitpid wait_child
#define access access_file
#include "desktop-session.cpp"
#undef waitpid
#undef access

void nce_android_command(std::initializer_list<const char *>, const char *) { ++commands; }
void nce_framework_check(const char *, unsigned) { assert(false); }
bool nce_surface_check(const char *package, const char *, bool cancellable) {
    assert(strcmp(package, "com.android.launcher3") == 0 && cancellable);
    ++surface_calls;
    if (stop_after_surface) stop = true;
    return !surface_cancel;
}
static pid_t child_process(int code) {
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        if (code == -1) for (;;) pause();
        if (code == -2) { raise(SIGTERM); _exit(99); }
        _exit(code);
    }
    return child;
}
static void desktop_check(bool cancelled, bool stop_after, bool expected) {
    stop = false; surface_cancel = cancelled; stop_after_surface = stop_after;
    commands = surface_calls = 0;
    FILE *output = tmpfile();
    assert(output);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fileno(output), STDOUT_FILENO) >= 0);
    assert(nce_desktop_start(true) == expected);
    fflush(stdout);
    assert(dup2(saved, STDOUT_FILENO) >= 0 && close(saved) == 0);
    rewind(output);
    char text[2048] = {};
    fread(text, 1, sizeof(text) - 1, output);
    fclose(output);
    assert((strstr(text, "android_desktop_ready") != nullptr) == expected);
    assert(strstr(text, "android_desktop_first_frame_wait"));
    assert(commands == 3 && surface_calls == 1);
}
int main() {
    stop = false;
    interruptions = 1;
    int status = nce_wait_desktop_child(child_process(0), true);
    assert(status == 0 && interruptions == 0);
    status = nce_wait_desktop_child(child_process(7), false);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    status = nce_wait_desktop_child(child_process(-2), true);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    stop = true;
    reap_interruptions = 1;
    pid_t child = child_process(-1);
    assert(nce_wait_desktop_child(child, true) == -ECANCELED && reap_interruptions == 0);
    assert(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(nce_wait_desktop_child(child, true) == -ECHILD);
    assert(nce_wait_desktop_child(child_process(0), false) == 0);
    desktop_check(true, false, false);
    desktop_check(false, true, false);
    desktop_check(false, false, true);
    puts("desktop wait: nine checks passed");
}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'main.cpp').write_text(SOURCE)
    subprocess.run(['xcrun', 'clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', ROOT / 'experiments/native-linux/userspace', OUT / 'main.cpp', '-o', OUT / 'check'],
                   check=True, timeout=60)
    subprocess.run([OUT / 'check'], check=True, timeout=15)


if __name__ == '__main__':
    main()
