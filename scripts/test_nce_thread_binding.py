#!/usr/bin/env python3
"""检查 NCE 线程绑定遇到系统 API 失败或 TSD 布局不匹配时的行为。"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'work/native-linux-nce/thread-binding-checks'
SOURCE = r'''
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
static int mode, deleted, written, read_count;
static void *slot;
static int create(pthread_key_t *key, void (*destructor)(void *)) {
    assert(!destructor);
    *key = mode == 1 ? 4096 : 256;
    return mode == 2 ? EAGAIN : 0;
}
static int destroy(pthread_key_t key) { (void)key; ++deleted; return 0; }
static int set(pthread_key_t key, const void *value) {
    assert(key == 256);
    ++written;
    if (mode == 3 && value) return ENOMEM;
    slot = (void *)value;
    return 0;
}
static void *get(pthread_key_t key) { assert(key == 256); return slot; }
static kern_return_t read_slot(vm_map_read_t map, vm_address_t from,
                               vm_size_t size, vm_address_t to,
                               vm_size_t *copied) {
    assert(map == mach_task_self() && from && size == sizeof(void *));
    ++read_count;
    if (mode == 4) return KERN_INVALID_ADDRESS;
    *copied = mode == 5 ? 0 : size;
    *(void **)to = mode == 6 ? NULL : slot;
    return KERN_SUCCESS;
}
#define pthread_key_create create
#define pthread_key_delete destroy
#define pthread_setspecific set
#define pthread_getspecific get
#define vm_read_overwrite read_slot
#include "nce-thread.c"
int main(int argc, char **argv) {
    assert(argc == 2);
    mode = atoi(argv[1]);
    unsigned key;
    if (mode == 7) {
        mode = 0;
        assert(nce_thread_key(&key) == 0 && key == 256);
        NceState state = {0};
        mode = 6;
        assert(nce_thread_bind(&state) == ENOTSUP);
        assert(deleted == 0);
    } else {
        int expected = mode == 2 ? EAGAIN : mode == 3 ? ENOMEM : ENOTSUP;
        assert(nce_thread_key(&key) == expected);
        assert(deleted == (mode == 2 ? 0 : 1));
        int prior_written = written, prior_read = read_count;
        assert(nce_thread_bind(NULL) == expected);
        assert(written == prior_written && read_count == prior_read);
        if (mode <= 3) assert(read_count == 0);
    }
}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'check.c').write_text(SOURCE)
    subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', '-I',
                    str(ROOT / 'experiments/native-linux'), str(OUT / 'check.c'), '-o', str(OUT / 'check')],
                   check=True, timeout=30)
    for mode in range(1, 8):
        subprocess.run([OUT / 'check', str(mode)], check=True, timeout=5)
    print('thread binding failure boundaries: seven checks passed')


if __name__ == '__main__':
    main()
