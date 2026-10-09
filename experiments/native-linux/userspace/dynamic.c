/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "dynamic:%d errno=%d\n", __LINE__, errno); \
            _exit(__LINE__); \
        } \
    } while (0)
int dependency_add(int);
int dependency_ready(void);
static _Thread_local int main_tls = 41;
static int (*plugin_add)(int);
static int images;
static volatile sig_atomic_t caught;

static int count_image(struct dl_phdr_info *info, size_t size, void *unused)
{
    (void)unused;
    CHECK(size >= sizeof(*info) && info->dlpi_phnum > 0 && info->dlpi_phdr);
    ++images;
    return 0;
}
static void signal_handler(int signal)
{
    CHECK(signal == SIGUSR1);
    caught = 1;
}
static void *thread(void *opaque)
{
    int id = (int)(intptr_t)opaque;
    CHECK(main_tls == 41 && dependency_add(id) == 7 + id && plugin_add(id) == 100 + id);
    main_tls = id;
    for (int i = 0; i < 8; ++i) {
        CHECK(dependency_add(1) == 8 + id + i);
        CHECK(plugin_add(1) == 101 + id + i);
        CHECK(main_tls == id);
        sched_yield();
    }
    return opaque;
}

int main(void)
{
    CHECK(dependency_ready() == 19 && dependency_add(2) == 9);
    void *library = dlopen("/lib/plugin.so", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        dprintf(2, "dlopen: %s\n", dlerror());
    CHECK(library);
    plugin_add = dlsym(library, "plugin_add");
    CHECK(plugin_add && plugin_add(3) == 103);
    CHECK(dlsym(library, "missing_symbol") == NULL && dlerror());
    CHECK(dl_iterate_phdr(count_image, NULL) == 0 && images >= 4);
    puts("{\"test\":\"linux_dynamic_link_dlopen_constructors\",\"passed\":true}");

    pthread_t threads[8];
    for (int i = 0; i < 8; ++i)
        CHECK(pthread_create(&threads[i], NULL, thread, (void *)(intptr_t)(i + 1)) == 0);
    for (int i = 0; i < 8; ++i) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0 && (intptr_t)result == i + 1);
    }
    CHECK(main_tls == 41 && dependency_add(0) == 9 && plugin_add(0) == 103);
    puts("{\"test\":\"linux_dynamic_tls_threads\",\"passed\":true}");

    CHECK(signal(SIGUSR1, signal_handler) != SIG_ERR && raise(SIGUSR1) == 0 && caught);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(main_tls == 41 && dependency_add(1) == 10 && plugin_add(1) == 104);
        _exit(17);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 17);
    CHECK(dependency_add(0) == 9 && plugin_add(0) == 103);
    CHECK(dlclose(library) == 0);
    puts("{\"test\":\"linux_dynamic_fork_signal\",\"passed\":true}");
    return 0;
}
