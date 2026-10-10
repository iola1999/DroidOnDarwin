/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local unsigned value = 17;
static pthread_barrier_t thread_barrier;

static void *waiting_thread(void *pointer)
{
    jmp_buf state;
    int result = setjmp(state);
    if (!result)
        longjmp(state, 7);
    if (result != 7 || value != 17)
        abort();
    pthread_barrier_wait(&thread_barrier);
    return pointer;
}

static unsigned long virtual_kib(void)
{
    FILE *file = fopen("/proc/self/status", "r");
    if (!file)
        abort();
    char line[256];
    unsigned long kib = 0;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "VmSize: %lu", &kib) == 1)
            break;
    if (fclose(file) || !kib)
        abort();
    return kib;
}

static void many_threads(void)
{
    enum { count = 128 };
    pthread_t threads[count];
    if (pthread_barrier_init(&thread_barrier, NULL, count + 1))
        abort();
    unsigned long before = virtual_kib();
    for (unsigned i = 0; i < count; ++i)
        if (pthread_create(&threads[i], NULL, waiting_thread, &threads[i]))
            abort();
    unsigned long after = virtual_kib();
    // Include ordinary stacks, TLS and shadow stacks in the reservation limit.
    if (after < before || after - before >= count * 4096UL)
        abort();
    pthread_barrier_wait(&thread_barrier);
    for (unsigned i = 0; i < count; ++i) {
        void *result;
        if (pthread_join(threads[i], &result) || result != &threads[i])
            abort();
    }
    if (pthread_barrier_destroy(&thread_barrier))
        abort();
    printf("{\"test\":\"android_bionic_many_threads\",\"passed\":true,\"threads\":%u,\"reserved_kib\":%lu}\n",
           count, after - before);
}

static void *worker(void *pointer)
{
    if (value != 17)
        abort();
    value = 29;
    memset(pointer, 0xa5, 65536);
    return pointer;
}

void file_tests(const char *libc);
void signal_tests(const char *libc);
void seccomp_tests(void);
void proc_exit_tests(void);

static void allocator_class_growth(void)
{
    /* Exceed one 32 MiB Scudo size class and exercise its allocation fallback. */
    enum { count = 1024,
           bytes = 49152 };
    unsigned char **blocks = calloc(count, sizeof(*blocks));
    if (!blocks)
        abort();
    for (unsigned i = 0; i < count; ++i) {
        blocks[i] = malloc(bytes);
        if (!blocks[i])
            abort();
        memset(blocks[i], (unsigned char)i, bytes);
    }
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned j = 0; j < bytes; ++j)
            if (blocks[i][j] != (unsigned char)i)
                abort();
        free(blocks[i]);
    }
    free(blocks);
    puts("{\"test\":\"android_scudo_class_growth\",\"passed\":true,\"allocated_bytes\":50331648}");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    file_tests("bionic");
    signal_tests("bionic");
    seccomp_tests();
    pthread_t thread;
    void *result;
    unsigned char *memory = malloc(65536);
    if (!memory || pthread_create(&thread, NULL, worker, memory) ||
        pthread_join(thread, &result) || result != memory || value != 17)
        return 1;
    for (unsigned i = 0; i < 65536; ++i)
        if (memory[i] != 0xa5)
            return 2;
    free(memory);
    jmp_buf saved;
    int jumped = setjmp(saved);
    if (!jumped)
        longjmp(saved, 7);
    if (jumped != 7)
        return 3;
    allocator_class_growth();
    many_threads();
    proc_exit_tests();
    printf("{\"test\":\"android_bionic_threads_allocator_tls\",\"passed\":true,\"page_size\":%ld}\n",
           sysconf(_SC_PAGESIZE));
    return 0;
}
