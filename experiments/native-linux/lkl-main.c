/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce.h"
#ifdef NCE_PROCESS
#include "memory.h"
#include "layout.h"
void nce_compute_benchmark(void);
#endif
#include "lkl-host-ops.h"
#include "lkl-image.h"
#include "guest-image.h"
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } } while (0)
extern struct lkl_host_operations nce_lkl_host;
static long (*kernel_syscall)(long, long *);

#ifndef NCE_PROCESS
static void *mapped(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(p != MAP_FAILED);
    return p;
}
static NceCode dual(size_t size)
{
    void *rw = mapped(size);
    vm_address_t rx = 0;
    vm_prot_t current, maximum;
    REQUIRE(vm_remap(mach_task_self(), &rx, size, 0, VM_FLAGS_ANYWHERE,
                    mach_task_self(), (uintptr_t)rw, FALSE, &current, &maximum,
                    VM_INHERIT_NONE) == KERN_SUCCESS);
    return (NceCode){rw, (void *)rx, size};
}
static void release(NceCode code)
{
    REQUIRE(munmap(code.rw, code.size) == 0);
    REQUIRE(munmap(code.rx, code.size) == 0);
}
#endif
static uint64_t now(void)
{
    struct timespec t;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

#ifndef NCE_PROCESS
static void linux_syscall(NceState *s)
{
    if (s->regs.x[8] == 93) { s->done = 1; return; }
    long args[6];
    for (unsigned i = 0; i < 6; ++i) args[i] = (long)s->regs.x[i];
    s->regs.x[0] = kernel_syscall(s->regs.x[8], args);
}

struct waiter { int fd; long result, tid; unsigned value; atomic_int ready; };
static void *pipe_wait(void *opaque)
{
    struct waiter *w = opaque;
    long args[6] = {w->fd, (long)&w->value, sizeof(w->value)};
    atomic_store(&w->ready, 1);
    w->result = kernel_syscall(63, args);
    w->tid = kernel_syscall(178, args);
    return NULL;
}
static void *futex_wait(void *opaque)
{
    struct waiter *w = opaque;
    long args[6] = {(long)&w->value, 128, 0};
    atomic_store(&w->ready, 1);
    w->result = kernel_syscall(98, args);
    return NULL;
}
static void blocking_tests(void)
{
    int fds[2];
    long args[6] = {(long)fds, 0};
    REQUIRE(kernel_syscall(59, args) == 0);
    struct waiter w = {.fd = fds[0]};
    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, pipe_wait, &w) == 0);
    while (!atomic_load(&w.ready)) sched_yield();
    struct timespec delay = {.tv_nsec = 1000000};
    nanosleep(&delay, NULL);
    unsigned value = 0x73524102;
    args[0] = fds[1]; args[1] = (long)&value; args[2] = sizeof(value);
    REQUIRE(kernel_syscall(64, args) == sizeof(value));
    REQUIRE(pthread_join(thread, NULL) == 0);
    REQUIRE(w.result == sizeof(value) && w.value == value);
    REQUIRE(w.tid != kernel_syscall(178, args));
    args[0] = fds[0]; REQUIRE(kernel_syscall(57, args) == 0);
    args[0] = fds[1]; REQUIRE(kernel_syscall(57, args) == 0);
    puts("{\"test\":\"linux_pipe_host_threads\",\"passed\":true}");

    struct waiter f = {0};
    REQUIRE(pthread_create(&thread, NULL, futex_wait, &f) == 0);
    while (!atomic_load(&f.ready)) sched_yield();
    memset(args, 0, sizeof(args));
    args[0] = (long)&f.value; args[1] = 129; args[2] = 1;
    uint64_t deadline = now() + 2000000000;
    long awakened;
    do {
        awakened = kernel_syscall(98, args);
        REQUIRE(awakened >= 0 && now() < deadline);
        if (!awakened) sched_yield();
    } while (!awakened);
    REQUIRE(awakened == 1 && pthread_join(thread, NULL) == 0 && f.result == 0);
    puts("{\"test\":\"linux_futex_wait_wake\",\"passed\":true}");
}

#else
unsigned long nce_process_setup(void);
void nce_process_cleanup(void);
void nce_process_tests(long (*)(long, long *), long (*)(long, long *), int (*)(unsigned long, int),
                       void (*)(long, long, unsigned long), void (*)(unsigned long, int));
#endif

int nce_lkl_run(const char *image_path)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    #ifdef NCE_PROCESS
    if (nce_memory_prepare() != 0) return 1;
    nce_compute_benchmark();
    NceCode kernel = nce_memory_take(LKL_IMAGE_SIZE);
    #else
    NceCode kernel = dual(LKL_IMAGE_SIZE);
    #endif
    puts("{\"phase\":\"load_linux_image\"}");
    FILE *file = fopen(image_path, "rb");
    REQUIRE(file);
    REQUIRE(fread(kernel.rw, 1, LKL_IMAGE_SIZE, file) == LKL_IMAGE_SIZE);
    REQUIRE(fgetc(file) == EOF);
    REQUIRE(fclose(file) == 0);
    for (size_t i = 0; i < sizeof(lkl_relocations) / sizeof(lkl_relocations[0]); ++i) {
        uint64_t value = (uintptr_t)kernel.rx + lkl_relocations[i][1];
        memcpy(kernel.rw + lkl_relocations[i][0], &value, sizeof(value));
    }
    for (size_t i = 0; i < sizeof(lkl_segments) / sizeof(lkl_segments[0]); ++i) {
        int protection = (int)lkl_segments[i][2];
        REQUIRE(mprotect(kernel.rx + lkl_segments[i][0], lkl_segments[i][1], protection) == 0);
    }
    nce_sync_code(kernel);
    int (*init)(struct lkl_host_operations *) = (void *)(kernel.rx + LKL_INIT);
    int (*start)(void) = (void *)(kernel.rx + NCE_LKL_START);
    long (*halt)(void) = (void *)(kernel.rx + LKL_SYS_HALT);
    void (*cleanup)(void) = (void *)(kernel.rx + LKL_CLEANUP);
    kernel_syscall = (void *)(kernel.rx + LKL_SYSCALL);
    #ifdef NCE_PROCESS
    void (*set_layout)(unsigned long, unsigned long) = (void *)(kernel.rx + NCE_SET_LAYOUT);
    const char *mode = getenv("NCE_MM_MODE");
    if (!mode) mode = "lazy";
    REQUIRE(!strcmp(mode, "full") || !strcmp(mode, "diff") || !strcmp(mode, "lazy"));
    unsigned long mapping_mode = !strcmp(mode, "full") ? NCE_MM_FULL : !strcmp(mode, "diff") ? NCE_MM_DIFF : NCE_MM_LAZY;
    printf("{\"phase\":\"linux_mapping_policy\",\"mode\":\"%s\"}\n", mode);
    set_layout(nce_process_setup(), mapping_mode);
    #endif
    puts("{\"phase\":\"linux_init\"}");
    REQUIRE(init(&nce_lkl_host) == 0);
    uint64_t started = now();
    REQUIRE(start() == 0);
    printf("{\"test\":\"native_linux_boot\",\"passed\":true,\"ms\":%.3f}\n", (now() - started) / 1e6);
    char uts[390] = {0};
    long args[6] = {(long)uts};
    REQUIRE(kernel_syscall(160, args) == 0);
    REQUIRE(strcmp(uts, "Linux") == 0);
    printf("{\"test\":\"linux_uname\",\"sysname\":\"%s\",\"release\":\"%s\"}\n", uts, uts + 130);

    #ifdef NCE_PROCESS
    nce_process_tests(kernel_syscall, (void *)(kernel.rx + NCE_RUN_SYSCALL),
                      (void *)(kernel.rx + NCE_PAGE_FAULT), (void *)(kernel.rx + NCE_USER_RETURN),
                      (void *)(kernel.rx + NCE_USER_FAULT));
    #else
    NceCode code = dual(65536);
    REQUIRE(mprotect(code.rx, code.size, PROT_READ | PROT_EXEC) == 0);
    void *stack = mapped(65536);
    NceState state = {.regs.sp = (uintptr_t)stack + 65536, .syscall = linux_syscall};
    REQUIRE(nce_prepare(code, &state, guest_image, sizeof(guest_image), GUEST_LINUX_FILE) == 0);
    nce_enter(&state);
    REQUIRE(state.done && state.regs.x[0] == 0 && state.calls == 7);
    puts("{\"test\":\"nce_linux_file_and_timer\",\"passed\":true}");
    blocking_tests();
    const unsigned iterations = 100000;
    started = now();
    long pid = 0;
    for (unsigned i = 0; i < iterations; ++i) pid = kernel_syscall(172, args);
    REQUIRE(pid > 0);
    printf("{\"bench\":\"native_linux_getpid\",\"iterations\":%u,\"ns_per_call\":%.2f}\n",
           iterations, (double)(now() - started) / iterations);
    memset(&state, 0, sizeof(state));
    state.regs.sp = (uintptr_t)stack + 65536; state.syscall = linux_syscall;
    REQUIRE(nce_prepare(code, &state, guest_image, sizeof(guest_image), GUEST_WORK) == 0);
    uint64_t data = 7;
    state.regs.x[0] = iterations; state.regs.x[1] = (uintptr_t)&data; state.regs.x[2] = 1;
    started = now();
    nce_enter(&state);
    REQUIRE(state.calls == iterations + 1);
    REQUIRE(state.regs.x[0] == (data ^ ((uint64_t)pid * iterations)));
    printf("{\"bench\":\"nce_linux_getpid_with_compute\",\"iterations\":%u,\"ns_per_iteration\":%.2f}\n",
           iterations, (double)(now() - started) / iterations);
    #endif
    puts("{\"phase\":\"linux_shutdown\"}");
    REQUIRE(halt() == 0);
    cleanup();
    #ifdef NCE_PROCESS
    nce_process_cleanup();
    #else
    release(code);
    REQUIRE(munmap(stack, 65536) == 0);
    #endif
    #ifdef NCE_PROCESS
    if (nce_memory_release()) return 1;
    #else
    release(kernel);
    #endif
    puts("{\"result\":\"passed\"}");
    return 0;
}

#ifndef NCE_IOS_APP
int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    return nce_lkl_run(argv[1]);
}
#endif
