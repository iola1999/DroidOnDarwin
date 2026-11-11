/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

static _Thread_local unsigned value = 17;
static volatile unsigned char zeroes[16387];

static int loaded(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    (void)data;
    printf("{\"component_object\":\"%s\",\"base\":%lu}\n", info->dlpi_name, (unsigned long)info->dlpi_addr);
    return 0;
}

static void *worker(void *argument)
{
    if (value != 17)
        return NULL;
    value = 29;
    return argument;
}

static int check(const char *name, int success)
{
    printf("{\"component_check\":\"%s\",\"success\":%s}\n", name, success ? "true" : "false");
    return success;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("{\"component_main\":true,\"aux_page_size\":%lu,\"page_size\":%ld}\n", getauxval(AT_PAGESZ), sysconf(_SC_PAGESIZE));
    dl_iterate_phdr(loaded, NULL);
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        while (fgets(line, sizeof(line), maps))
            fputs(line, stderr);
        fclose(maps);
    }
    int cleared = 1;
    for (unsigned i = 0; i < sizeof(zeroes); ++i)
        cleared &= zeroes[i] == 0;
    if (!check("page_bss", cleared && getauxval(AT_PAGESZ) == 16384 && sysconf(_SC_PAGESIZE) == 16384))
        return 1;
    unsigned char *memory = malloc(65536);
    if (!check("allocation", memory != NULL))
        return 2;
    memset(memory, 0xa5, 65536);
    volatile unsigned char *observed = memory;
    int valid = 1;
    for (unsigned i = 0; i < 65536; ++i)
        valid &= observed[i] == 0xa5;
    free(memory);
    if (!check("allocation_contents", valid))
        return 3;
    pthread_t thread;
    void *result = NULL;
    int error = pthread_create(&thread, NULL, worker, &value);
    if (!error)
        error = pthread_join(thread, &result);
    if (!check("thread_tls", !error && result == &value && value == 17))
        return 4;
    void *library = dlopen("libm.so", RTLD_NOW | RTLD_LOCAL);
    double (*cosine)(double) = library ? (double (*)(double))dlsym(library, "cos") : NULL;
    volatile double argument = 0;
    int math_ok = cosine && cosine(argument) == 1.0;
    if (library)
        math_ok &= dlclose(library) == 0;
    if (!check("dynamic_libm", math_ok))
        return 5;
    return 0;
}
