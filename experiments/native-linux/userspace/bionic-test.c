/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local unsigned value = 17;

static void *worker(void *pointer)
{
    if (value != 17) abort();
    value = 29;
    memset(pointer, 0xa5, 65536);
    return pointer;
}

void file_tests(const char *libc);
void signal_tests(const char *libc);
void seccomp_tests(void);

int main(void)
{
    file_tests("bionic");
    signal_tests("bionic");
    seccomp_tests();
    pthread_t thread;
    void *result;
    unsigned char *memory = malloc(65536);
    if (!memory || pthread_create(&thread, NULL, worker, memory) ||
        pthread_join(thread, &result) || result != memory || value != 17) return 1;
    for (unsigned i = 0; i < 65536; ++i) if (memory[i] != 0xa5) return 2;
    free(memory);
    jmp_buf saved;
    int jumped = setjmp(saved);
    if (!jumped) longjmp(saved, 7);
    if (jumped != 7) return 3;
    printf("{\"test\":\"android_bionic_threads_allocator_tls\",\"passed\":true,\"page_size\":%ld}\n",
           sysconf(_SC_PAGESIZE));
    return 0;
}
