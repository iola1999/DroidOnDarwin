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
extern int nce_disk_open(unsigned index, const char *path);
extern void nce_disk_close(void);
static long (*call)(long, long *);

static void directory(const char *path)
{
    long args[6] = {-100, (long)path, 0755};
    long result = call(34, args);
    REQUIRE(result == 0 || result == -17);
}

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

static void android_setup(void)
{
    const char *directories[] = {"/sys", "/system-image", "/system", "/apex", "/system_ext", "/product", "/vendor",
                                 "/data", "/data/nce", "/data/dalvik-cache", "/data/dalvik-cache/arm64", "/linkerconfig"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        directory(directories[i]);
    long mount[6] = {(long)"sysfs", (long)"/sys", (long)"sysfs"};
    REQUIRE(call(40, mount) == 0);
    long args[6] = {-100, (long)"/sys/block/ncedisk1/dev", 0};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    char device[64] = {0};
    args[0] = fd;
    args[1] = (long)device;
    args[2] = sizeof(device) - 1;
    REQUIRE(call(63, args) > 0 && call(57, args) == 0);
    unsigned major, minor;
    REQUIRE(sscanf(device, "%u:%u", &major, &minor) == 2 && major < 4096 && minor < 256);
    long node[6] = {-100, (long)"/dev/ncesystem", 0060400, (major << 8) | minor};
    REQUIRE(call(33, node) == 0);
    long system[6] = {(long)"/dev/ncesystem", (long)"/system-image", (long)"ext4", 1};
    REQUIRE(call(40, system) == 0);
    const char *targets[] = {"/system", "/apex", "/system_ext", "/product", "/vendor"};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(*targets); ++i) {
        char source[128];
        REQUIRE(snprintf(source, sizeof(source), "/system-image%s", targets[i]) < (int)sizeof(source));
        long bind[6] = {(long)source, (long)targets[i], 0, 4096};
        REQUIRE(call(40, bind) == 0);
    }
    long readonly[6] = {-100, (long)"/system/write-test", 0x241, 0600};
    REQUIRE(call(56, readonly) == -30);
    puts("{\"test\":\"android_system_image_readonly\",\"passed\":true}");
    const char *devices[] = {"/dev/null", "/dev/random", "/dev/urandom"};
    const unsigned numbers[] = {259, 264, 265};
    for (unsigned i = 0; i < 3; ++i) {
        long create[6] = {-100, (long)devices[i], 0020666, numbers[i]};
        REQUIRE(call(33, create) == 0);
    }
    struct {
        int entropy_bits, bytes;
        unsigned char data[32];
    } entropy = {256, 32, {0}};
    arc4random_buf(entropy.data, sizeof(entropy.data));
    long random[6] = {-100, (long)"/dev/random", 2};
    fd = call(56, random);
    REQUIRE(fd >= 0);
    random[0] = fd;
    random[1] = 0x40085203;
    random[2] = (long)&entropy;
    REQUIRE(call(29, random) == 0 && call(57, random) == 0);
    put_file("/linkerconfig/ld.config.txt", "ld.config.txt");
    put_file("/data/nce/NceWorkload.dex", "NceWorkload.dex");
    const char *settings[] = {"art-nterp", "art-expanded-jit", "art-fast-baseline"};
    for (unsigned i = 0; i < sizeof(settings) / sizeof(*settings); ++i) {
        char path[128];
        REQUIRE(snprintf(path, sizeof(path), "/data/nce/%s", settings[i]) < (int)sizeof(path));
        put_file(path, settings[i]);
    }
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

static int run_program(const char *path)
{
    const char *streams[] = {"/stdin", "/stdout", "/stderr"};
    for (long fd = 0; fd < 3; ++fd) {
        long args[6] = {-100, (long)streams[fd], 0x242, 0600};
        long opened = call(56, args);
        REQUIRE(opened >= 0);
        if (opened != fd) {
            long dup[6] = {opened, fd, 0};
            REQUIRE(call(24, dup) == fd && call(57, dup) == 0);
        }
    }
    long launch[6] = {(long)translation_linux_process(path), 1};
    long pid = call(246, launch);
    REQUIRE(pid > 0);
    int status = -1;
    long wait[6] = {pid, (long)&status};
    long waited = call(260, wait);
    dump_file("/stdout", stdout);
    dump_file("/stderr", stderr);
    printf("{\"test\":\"translated_linux_process\",\"path\":\"%s\",\"passed\":%s,\"waited\":%ld,\"status\":%d}\n",
           path, waited == pid && status == 0 ? "true" : "false", waited, status);
    return waited != pid || status != 0;
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
    const char *workload = getenv("NCE_TRANSLATION_WORKLOAD");
    REQUIRE(!workload || !strcmp(workload, "core") || !strcmp(workload, "android"));
    int android = workload && !strcmp(workload, "android");
    if (android) {
        REQUIRE(getenv("NCE_TRANSLATION_SYSTEM"));
        REQUIRE(nce_disk_open(1, getenv("NCE_TRANSLATION_SYSTEM")) == 0);
    }
    REQUIRE(init(&nce_lkl_host) == 0);
    unsigned long slice;
    REQUIRE(start(0, &slice) == 0);
    printf("{\"phase\":\"translated_linux_boot\",\"slice_ns\":%lu}\n", slice);
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
    int failed = 0;
    if (android) {
        android_setup();
        const char *programs[] = {"/bionic-test", "/system/bin/cfi-test", "/system/bin/art-test"};
        const char *phases[] = {"bionic_execution", "cfi_execution", "art_execution"};
        for (unsigned i = 0; i < 3 && !failed; ++i) {
            translation_memory_phase(phases[i]);
            failed = run_program(programs[i]);
        }
    } else {
        translation_memory_phase("guest_execution");
        failed = run_program("/probe-guest");
    }
    memory_sample("guest_finished");
    translation_memory_phase("cleanup");
    REQUIRE(halt() == 0);
    cleanup();
    nce_disk_close();
    translation_linux_cleanup();
    REQUIRE(munmap(kernel, LKL_IMAGE_SIZE) == 0);
    if (!TRANSLATION_LOOKUP)
        free(views);
    memory_sample("cleaned");
    translation_memory_stop();
    puts(failed ? "{\"result\":\"failed\"}" : "{\"result\":\"passed\"}");
    return failed;
}

#ifndef TRANSLATION_DISPATCH
int main(int argc, char **argv)
{
    return argc == 2 ? nce_lkl_run(argv[1]) : 2;
}
#endif
