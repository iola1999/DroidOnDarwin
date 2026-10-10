/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "linux-execution.h"
#include "linux-memory.h"
#include "layout.h"
#include "lkl-host-ops.h"
#include "lkl-image.h"
#include "translation-symbols.h"
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define REQUIRE(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "runner:%d: %s\n", __LINE__, #c); \
            abort(); \
        } \
    } while (0)
extern struct lkl_host_operations nce_lkl_host;
static long (*call)(long, long *);

static void memory_sample(const char *phase)
{
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    REQUIRE(task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS);
    printf("{\"phase\":\"%s\",\"footprint_bytes\":%llu,\"resident_bytes\":%llu,\"resident_peak_bytes\":%llu}\n",
           phase, info.phys_footprint, info.resident_size, info.resident_size_peak);
}

static void put_file(const char *guest, const char *file)
{
    const char *directory = getenv("NCE_TRANSLATION_FILES");
    REQUIRE(directory);
    char path[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/%s", directory, file) < (int)sizeof(path));
    FILE *input = fopen(path, "rb");
    REQUIRE(input);
    long args[6] = {-100, (long)guest, 0x241, 0755};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    unsigned char buffer[65536];
    size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), input))) {
        args[0] = fd;
        args[1] = (long)buffer;
        args[2] = size;
        REQUIRE(call(64, args) == (long)size);
    }
    REQUIRE(!ferror(input) && fclose(input) == 0);
    args[0] = fd;
    REQUIRE(call(57, args) == 0);
}

static void dump_file(const char *path, FILE *output)
{
    long args[6] = {-100, (long)path, 0};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    char buffer[16384];
    args[0] = fd;
    args[1] = (long)buffer;
    args[2] = sizeof(buffer);
    long bytes;
    while ((bytes = call(63, args)) > 0)
        REQUIRE(fwrite(buffer, 1, bytes, output) == (size_t)bytes);
    REQUIRE(bytes == 0 && call(57, args) == 0);
}

int nce_lkl_run(const char *path)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *interval_text = getenv("NCE_TRANSLATION_MEMORY_MS");
    char *end = NULL;
    unsigned long interval = interval_text ? strtoul(interval_text, &end, 10) : 100;
    REQUIRE(!interval_text || (interval_text[0] >= '0' && interval_text[0] <= '9' && !*end));
    REQUIRE(interval == 0 || (interval >= 10 && interval <= 10000));
    translation_memory_start((unsigned)interval);
    memory_sample("before_kernel");
    unsigned char *kernel = mmap(NULL, LKL_IMAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(kernel != MAP_FAILED);
    FILE *input = fopen(path, "rb");
    REQUIRE(input && fread(kernel, 1, LKL_IMAGE_SIZE, input) == LKL_IMAGE_SIZE && fgetc(input) == EOF);
    REQUIRE(fclose(input) == 0);
    for (size_t i = 0; i < sizeof(lkl_relocations) / sizeof(*lkl_relocations); ++i)
        *(uintptr_t *)(kernel + lkl_relocations[i][0]) = (uintptr_t)kernel + lkl_relocations[i][1];
    for (size_t i = 0; i < sizeof(lkl_segments) / sizeof(*lkl_segments); ++i)
        REQUIRE(mprotect(kernel + lkl_segments[i][0], lkl_segments[i][1], lkl_segments[i][2]) == 0);
    sys_icache_invalidate(kernel, LKL_IMAGE_SIZE);
    int (*init)(struct lkl_host_operations *) = (void *)(kernel + LKL_INIT);
    int (*start)(unsigned long, unsigned long *) = (void *)(kernel + NCE_LKL_START);
    long (*halt)(void) = (void *)(kernel + LKL_SYS_HALT);
    void (*cleanup)(void) = (void *)(kernel + LKL_CLEANUP);
    call = (void *)(kernel + LKL_SYSCALL);
    void (*set_layout)(unsigned long, unsigned long, unsigned long *, unsigned long, void *, unsigned long, unsigned long) = (void *)(kernel + NCE_SET_LAYOUT);
    struct nce_translation_state state = {0};
    unsigned long *views = TRANSLATION_LOOKUP ? (void *)&state : calloc(2 * NCE_USER_SIZE / NCE_PAGE_SIZE, sizeof(*views));
    REQUIRE(views);
    unsigned long base = translation_linux_setup(kernel, TRANSLATION_COPY_USER, TRANSLATION_LOOKUP, TRANSLATION_LOOKUP ? &state : NULL);
    set_layout(base, TRANSLATION_LOOKUP ? NCE_MM_TRANSLATED : NCE_MM_DEMAND, views, 3, NULL, 1, 0);
    REQUIRE(init(&nce_lkl_host) == 0);
    unsigned long slice;
    REQUIRE(start(0, &slice) == 0);
    printf("{\"phase\":\"translated_linux_boot\",\"slice_ns\":%lu,\"cache_bytes\":16777216}\n", slice);
    memory_sample("kernel_ready");
    translation_memory_phase("guest_setup");
    long args[6] = {-100, (long)"/lib", 0755};
    REQUIRE(call(34, args) == 0);
    args[1] = (long)"/proc";
    REQUIRE(call(34, args) == 0);
    args[1] = (long)"/bin";
    REQUIRE(call(34, args) == 0);
    long mount[6] = {(long)"proc", (long)"/proc", (long)"proc"};
    REQUIRE(call(40, mount) == 0);
    put_file("/lib/nce-sigreturn.so", "sigreturn.elf");
    put_file("/probe-guest", "guest.elf");
    put_file("/probe-after-build", "late.elf");
    put_file("/dynamic", "dynamic.elf");
    put_file("/bin/sh", "dash.elf");
    put_file("/bionic-test", "bionic-test.elf");
    put_file("/lib/ld-musl-aarch64.so.1", "libc.so");
    put_file("/lib/dependency.so", "dependency.so");
    put_file("/lib/plugin.so", "plugin.so");
    const char *streams[] = {"/stdin", "/stdout", "/stderr"};
    for (long fd = 0; fd < 3; ++fd) {
        long open_args[6] = {-100, (long)streams[fd], 0x242, 0600};
        long opened = call(56, open_args);
        REQUIRE(opened >= 0);
        if (opened != fd) {
            long dup_args[6] = {opened, fd, 0};
            REQUIRE(call(24, dup_args) == fd && call(57, dup_args) == 0);
        }
    }
    translation_memory_phase("guest_execution");
    long launch[6] = {(long)translation_linux_process("/probe-guest"), 1};
    long pid = call(246, launch);
    REQUIRE(pid > 0);
    int status = -1;
    long wait[6] = {pid, (long)&status};
    long waited = call(260, wait);
    dump_file("/stdout", stdout);
    dump_file("/stderr", stderr);
    printf("{\"test\":\"translated_linux_process\",\"passed\":%s,\"waited\":%ld,\"status\":%d}\n", waited == pid && status == 0 ? "true" : "false", waited, status);
    memory_sample("guest_finished");
    translation_memory_phase("cleanup");
    REQUIRE(halt() == 0);
    cleanup();
    translation_linux_cleanup();
    REQUIRE(munmap(kernel, LKL_IMAGE_SIZE) == 0);
    if (!TRANSLATION_LOOKUP)
        free(views);
    memory_sample("cleaned");
    translation_memory_stop();
    puts(status == 0 && waited == pid ? "{\"result\":\"passed\"}" : "{\"result\":\"failed\"}");
    return status != 0 || waited != pid;
}

#ifndef TRANSLATION_DISPATCH
int main(int argc, char **argv)
{
    return argc == 2 ? nce_lkl_run(argv[1]) : 2;
}
#endif
