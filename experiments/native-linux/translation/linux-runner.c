/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "linux-execution.h"
#include "kernel-image.h"
#ifdef NCE_DIRECT_EXECUTION
#include "../direct/direct.h"
#endif
#include "linux-memory.h"
#include "layout.h"
#include "lkl-host-ops.h"
#include "lkl-image.h"
#include "translation-symbols.h"
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "ios-jit.h"
extern int nce_wait_debugger_detach(unsigned timeout_ms);
#endif
#ifdef NCE_GRAPHICS
#include "graphics/host-graphics.h"
#include "input.h"
#include "session.h"
#endif
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
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
extern int nce_disk_close(void);
static long (*call)(long, long *);
static unsigned char host_low_source[NCE_HOST_PAGE_SIZE] __attribute__((aligned(NCE_HOST_PAGE_SIZE)));

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

static void put_host_file(const char *guest, const char *path, unsigned mode)
{
    FILE *input = fopen(path, "rb");
    REQUIRE(input);
    long args[6] = {-100, (long)guest, 0x241, mode};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    unsigned char buffer[65536];
    size_t size;
    while ((size = fread(buffer, 1, sizeof(buffer), input))) {
        size_t done = 0;
        while (done < size) {
            args[0] = fd;
            args[1] = (long)(buffer + done);
            args[2] = size - done;
            long written = call(64, args);
            if (written == -4)
                continue;
            REQUIRE(written > 0 && (size_t)written <= size - done);
            done += written;
        }
    }
    REQUIRE(!ferror(input) && fclose(input) == 0);
    args[0] = fd;
    REQUIRE(call(57, args) == 0);
}

static void put_file(const char *guest, const char *file)
{
    const char *directory = getenv("NCE_TRANSLATION_FILES");
    REQUIRE(directory);
    char path[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/%s", directory, file) < (int)sizeof(path));
    put_host_file(guest, path, 0755);
}

static void put_text(const char *path, const char *value)
{
    long args[6] = {-100, (long)path, 0x241, 0644};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    args[0] = fd;
    args[1] = (long)value;
    args[2] = strlen(value);
    REQUIRE(call(64, args) == args[2] && call(57, args) == 0);
}

static void seed_entropy(void)
{
    struct {
        int entropy_bits, bytes;
        unsigned char data[32];
    } entropy = {256, 32, {0}};
    arc4random_buf(entropy.data, sizeof(entropy.data));
    long random[6] = {-100, (long)"/dev/random", 2};
    long fd = call(56, random);
    REQUIRE(fd >= 0);
    random[0] = fd;
    random[1] = 0x40085203;
    random[2] = (long)&entropy;
    REQUIRE(call(29, random) == 0 && call(57, random) == 0);
    unsigned char ready[32];
    long getrandom[6] = {(long)ready, sizeof(ready), 1};
    REQUIRE(call(278, getrandom) == (long)sizeof(ready));
    printf("{\"config\":\"linux_random_source\",\"seed_bits\":256,\"getrandom_bytes\":32,\"flags\":1}\n");
}

static void stock_bionic_setup(void)
{
    const char *directories[] = {"/dev", "/apex", "/apex/com.android.runtime", "/apex/com.android.runtime/bin",
                                 "/apex/com.android.runtime/lib64", "/apex/com.android.runtime/lib64/bionic",
                                 "/system", "/system/bin", "/linkerconfig"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        directory(directories[i]);
    const char *devices[] = {"/dev/null", "/dev/random", "/dev/urandom"};
    const unsigned numbers[] = {259, 264, 265};
    for (unsigned i = 0; i < sizeof(devices) / sizeof(*devices); ++i) {
        long create[6] = {-100, (long)devices[i], 0020666, numbers[i]};
        REQUIRE(call(33, create) == 0);
    }
    seed_entropy();
    const char *files[] = {"bin/linker64", "lib64/bionic/libc.so", "lib64/bionic/libdl.so", "lib64/bionic/libm.so"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(*files); ++i) {
        char guest[128], input[128];
        REQUIRE(snprintf(guest, sizeof(guest), "/apex/com.android.runtime/%s", files[i]) < (int)sizeof(guest));
        REQUIRE(snprintf(input, sizeof(input), "stock-runtime/%s", files[i]) < (int)sizeof(input));
        put_file(guest, input);
    }
    long link[6] = {(long)"/apex/com.android.runtime/bin/linker64", -100, (long)"/system/bin/linker64"};
    REQUIRE(call(36, link) == 0);
    put_file("/system/bin/stock-bionic", "bionic-test.elf");
    put_file("/linkerconfig/ld.config.txt", "stock-ld.config.txt");
}

#include "stock-art-runner.h"

static void apk_fixture_setup(int desktop)
{
    const char *input = getenv("NCE_APK_FIXTURE_DIR");
    const char *stage = getenv("NCE_APK_FIXTURE_STAGE");
    if (!input && !stage)
        return;
    REQUIRE(desktop && input && input[0] == '/' && stage);
    REQUIRE(!strcmp(stage, "install-update") || !strcmp(stage, "verify-restart"));
    directory("/data/nce/apk-fixture");
    const char *files[] = {"manifest.json", "java-v1.apk", "java-v2.apk", "jni-v1.apk", "jni-v2.apk"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(*files); ++i) {
        char guest[128], host[4096];
        REQUIRE(snprintf(guest, sizeof(guest), "/data/nce/apk-fixture/%s", files[i]) < (int)sizeof(guest));
        REQUIRE(snprintf(host, sizeof(host), "%s/%s", input, files[i]) < (int)sizeof(host));
        put_host_file(guest, host, 0600);
    }
    // Publish the stage after every input is available to the Android helper.
    put_text("/data/nce/apk-fixture/stage", stage);
}

static void block_device(const char *description, const char *path, unsigned mode)
{
    long args[6] = {-100, (long)description, 0};
    long fd = call(56, args);
    REQUIRE(fd >= 0);
    char device[64] = {0};
    args[0] = fd;
    args[1] = (long)device;
    args[2] = sizeof(device) - 1;
    REQUIRE(call(63, args) > 0 && call(57, args) == 0);
    unsigned major, minor;
    REQUIRE(sscanf(device, "%u:%u", &major, &minor) == 2 && major < 4096 && minor < 256);
    long node[6] = {-100, (long)path, 0060000 | mode, (major << 8) | minor};
    REQUIRE(call(33, node) == 0);
}

static void android_setup(int services, int desktop)
{
    const char *directories[] = {"/sys", "/system-image", "/system", "/apex", "/system_ext", "/product", "/vendor",
                                 "/data", "/data/nce", "/data/dalvik-cache", "/data/dalvik-cache/arm64", "/linkerconfig"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        directory(directories[i]);
    long mount[6] = {(long)"sysfs", (long)"/sys", (long)"sysfs"};
    REQUIRE(call(40, mount) == 0);
    block_device("/sys/block/ncedisk1/dev", "/dev/ncesystem", 0400);
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
    const char *devices[] = {"/dev/null", "/dev/random", "/dev/urandom", "/dev/kmsg", "/dev/fuse"};
    const unsigned numbers[] = {259, 264, 265, 267, (10 << 8) | 229};
    for (unsigned i = 0; i < sizeof(devices) / sizeof(*devices); ++i) {
        long create[6] = {-100, (long)devices[i], 0020666, numbers[i]};
        REQUIRE(call(33, create) == 0);
        long permissions[6] = {-100, (long)devices[i], 0666};
        REQUIRE(call(53, permissions) == 0);
    }
    seed_entropy();
    if (services) {
        const char *filesystems[][2] = {{"selinuxfs", "/sys/fs/selinux"}, {"fusectl", "/sys/fs/fuse/connections"}};
        for (unsigned i = 0; i < sizeof(filesystems) / sizeof(*filesystems); ++i) {
            long mount[6] = {(long)filesystems[i][0], (long)filesystems[i][1], (long)filesystems[i][0]};
            REQUIRE(call(40, mount) == 0);
        }
        block_device("/sys/block/ncedisk0/dev", "/dev/ncedisk0", 0600);
        long data[6] = {(long)"/dev/ncedisk0", (long)"/data", (long)"ext4", 1024};
        REQUIRE(call(40, data) == 0);
        long owner[6] = {-100, (long)"/data", 1000, 1000};
        REQUIRE(call(54, owner) == 0);
        long permissions[6] = {-100, (long)"/data", 0771};
        REQUIRE(call(53, permissions) == 0);
        directory("/data/nce");
        /* Session flags and generated fixtures have the same lifetime as the
         * kernel. Android application data remains on the ext4 mount. */
        long temporary[6] = {(long)"tmpfs", (long)"/data/nce", (long)"tmpfs", 0, (long)"mode=0755"};
        REQUIRE(call(40, temporary) == 0);
        directory("/data/dalvik-cache");
        directory("/data/dalvik-cache/arm64");
        put_file("/data/nce/nce-policy.cil", "nce-policy.cil");
        put_file("/data/nce/property_info", "property_info");
        put_text("/data/nce/art-aot", "0");
        put_text("/data/nce/art-boot-image", "0");
        put_text("/data/nce/art-compiler-profile", "0");
        if (desktop && getenv("NCE_DESKTOP"))
            put_text("/data/nce/desktop-session", "");
        if (desktop && getenv("NCE_DIAGNOSTICS"))
            put_text("/data/nce/diagnostics", "");
        if (desktop) {
            put_file("/data/nce/system-server-probe", "enabled");
            put_file("/data/nce/graphics", "enabled");
        }
        apk_fixture_setup(desktop);
        puts("{\"test\":\"android_data_ext4_mounted\",\"passed\":true}");
    }
    put_file("/linkerconfig/ld.config.txt", "ld.config.txt");
    put_file("/data/nce/NceWorkload.dex", "NceWorkload.dex");
    const char *settings[] = {"art-nterp", "art-expanded-jit", "art-fast-baseline"};
    const char *environment[] = {"NCE_ART_NTERP", "NCE_ART_EXPANDED_JIT", "NCE_ART_FAST_BASELINE"};
    const char *names[] = {"art_nterp", "art_expanded_jit", "art_fast_baseline"};
    for (unsigned i = 0; i < sizeof(settings) / sizeof(*settings); ++i) {
        char path[128];
        REQUIRE(snprintf(path, sizeof(path), "/data/nce/%s", settings[i]) < (int)sizeof(path));
        const char *value = getenv(environment[i]);
        if (!value)
            value = "0";
        REQUIRE(!strcmp(value, "0") || !strcmp(value, "1"));
        put_text(path, value);
        printf("{\"config\":\"%s\",\"requested\":%s}\n", names[i], value);
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

struct LogPump {
    atomic_bool stopping;
    pthread_mutex_t lock;
    pthread_cond_t changed;
};

static void *stream_log(void *opaque)
{
    struct LogPump *pump = opaque;
    char buffer[2][65536];
    size_t used[2] = {0};
    long offsets[2] = {0};
    for (;;) {
        int received = 0;
        for (unsigned i = 0; i < 2; ++i) {
            long args[6] = {i + 1, (long)(buffer[i] + used[i]), sizeof(buffer[i]) - used[i], offsets[i]};
            long count = call(67, args);
            REQUIRE(count >= 0);
            received |= count > 0;
            offsets[i] += count;
            used[i] += count;
            size_t complete = used[i];
            while (complete && buffer[i][complete - 1] != '\n')
                --complete;
            if (!count && atomic_load(&pump->stopping))
                complete = used[i];
            if (complete) {
                FILE *output = i ? stderr : stdout;
                // Keep each guest JSON line intact beside host memory samples.
                REQUIRE(fwrite(buffer[i], 1, complete, output) == complete && fflush(output) == 0);
                memmove(buffer[i], buffer[i] + complete, used[i] - complete);
                used[i] -= complete;
            }
            REQUIRE(used[i] < sizeof(buffer[i]));
        }
        if (received)
            continue;
        REQUIRE(pthread_mutex_lock(&pump->lock) == 0);
        if (atomic_load(&pump->stopping)) {
            REQUIRE(pthread_mutex_unlock(&pump->lock) == 0);
            return NULL;
        }
        struct timespec delay = {.tv_sec = 1};
        int error = pthread_cond_timedwait_relative_np(&pump->changed, &pump->lock, &delay);
        REQUIRE(error == 0 || error == ETIMEDOUT);
        REQUIRE(pthread_mutex_unlock(&pump->lock) == 0);
    }
}

static int run_program(const char *path, int live)
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
    struct LogPump pump = {.stopping = 0, .lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};
    pthread_t logger;
    if (live)
        REQUIRE(pthread_create(&logger, NULL, stream_log, &pump) == 0);
    long launch[6] = {(long)translation_linux_process(path), 1};
    long pid = call(246, launch);
    REQUIRE(pid > 0);
    int status = -1;
    long wait[6] = {pid, (long)&status};
    long waited = call(260, wait);
    if (live) {
        REQUIRE(pthread_mutex_lock(&pump.lock) == 0);
        atomic_store(&pump.stopping, 1);
        REQUIRE(pthread_cond_signal(&pump.changed) == 0 && pthread_mutex_unlock(&pump.lock) == 0);
        REQUIRE(pthread_join(logger, NULL) == 0);
    } else {
        dump_file("/stdout", stdout);
        dump_file("/stderr", stderr);
    }
    REQUIRE(pthread_cond_destroy(&pump.changed) == 0 && pthread_mutex_destroy(&pump.lock) == 0);
    printf("{\"test\":\"translated_linux_process\",\"path\":\"%s\",\"passed\":%s,\"waited\":%ld,\"status\":%d}\n",
           path, waited == pid && status == 0 ? "true" : "false", waited, status);
    return waited != pid || status != 0;
}

static int detach_preparation(void)
{
#if TARGET_OS_IPHONE
    dod_ios_jit_detach();
    return nce_wait_debugger_detach(5000);
#else
    return 0;
#endif
}

int nce_lkl_run(const char *path)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    unsigned long scheduler_slice_us = 0;
    const char *slice_text = getenv("NCE_SCHED_SLICE_US");
    if (slice_text) {
        char *end;
        errno = 0;
        scheduler_slice_us = strtoul(slice_text, &end, 10);
        if (errno || slice_text[0] < '0' || slice_text[0] > '9' || *end ||
            (scheduler_slice_us && (scheduler_slice_us < 100 || scheduler_slice_us > 100000))) {
            puts("{\"result\":\"failed\",\"reason\":\"scheduler_slice_invalid\"}");
            detach_preparation();
            return 1;
        }
    }
    const char *art_assets[] = {"NCE_ART_AOT", "NCE_ART_BOOT_IMAGE"};
    for (unsigned i = 0; i < sizeof(art_assets) / sizeof(*art_assets); ++i) {
        const char *value = getenv(art_assets[i]);
        if (value && strcmp(value, "0")) {
            puts("{\"result\":\"failed\",\"reason\":\"translation_art_assets_unavailable\"}");
            detach_preparation();
            return 1;
        }
    }
    const char *interval_text = getenv("NCE_TRANSLATION_MEMORY_MS");
    char *end = NULL;
    unsigned long interval = interval_text ? strtoul(interval_text, &end, 10) : 100;
    REQUIRE(!interval_text || (interval_text[0] >= '0' && interval_text[0] <= '9' && !*end));
    REQUIRE(interval == 0 || (interval >= 10 && interval <= 10000));
    translation_memory_start((unsigned)interval);
    memory_sample("before_kernel");
#ifdef NCE_TRANSLATION_DUAL_MAP
    const char *code_memory = "dual";
#else
    const char *code_memory = "single";
#endif
#ifdef NCE_DIRECT_EXECUTION
    const char *direct_memory = nce_direct_code_memory();
#ifdef NCE_TRANSLATION_DUAL_MAP
    REQUIRE(!strcmp(direct_memory, "dual"));
#else
    REQUIRE(!strcmp(direct_memory, "map-jit"));
#endif
#else
    const char *direct_memory = "unused";
#endif
    printf("{\"config\":\"translation_code_memory\",\"kernel\":\"%s\",\"direct\":\"%s\"}\n", code_memory, direct_memory);
    TranslationKernelImage loaded = {0};
    if (translation_kernel_load(&loaded, path)) {
        printf("{\"result\":\"failed\",\"reason\":\"translation_kernel_load\",\"errno\":%d}\n", errno);
        detach_preparation();
        translation_memory_stop();
        return 1;
    }
    unsigned char *kernel = loaded.base;
    int (*init)(struct lkl_host_operations *) = (void *)(kernel + LKL_INIT);
#if TRANSLATION_LOOKUP
    int (*start)(unsigned long, unsigned long *, unsigned) = (void *)(kernel + NCE_LKL_START_CPUS);
#else
    int (*start)(unsigned long, unsigned long *) = (void *)(kernel + NCE_LKL_START);
#endif
    long (*halt)(void) = (void *)(kernel + LKL_SYS_HALT);
    void (*cleanup)(void) = (void *)(kernel + LKL_CLEANUP);
    call = (void *)(kernel + LKL_SYSCALL);
    void (*set_layout)(unsigned long, unsigned long, unsigned long *, unsigned long, void *, unsigned long, unsigned long) = (void *)(kernel + NCE_SET_LAYOUT);
    struct nce_translation_state state = {0};
    unsigned long *views = TRANSLATION_LOOKUP ? (void *)&state : calloc(2 * NCE_USER_SIZE / NCE_PAGE_SIZE, sizeof(*views));
    unsigned long guest_base = views ? translation_linux_setup(kernel, TRANSLATION_COPY_USER, TRANSLATION_LOOKUP, TRANSLATION_LOOKUP ? &state : NULL) : 0;
    int preparation_failed = detach_preparation();
#ifdef NCE_TRANSLATION_DUAL_MAP
    if (guest_base && !preparation_failed)
        preparation_failed = translation_kernel_check_code(&loaded) || translation_linux_check_code();
    printf("{\"test\":\"translation_prepared_code_execution\",\"passed\":%s}\n",
           guest_base && !preparation_failed ? "true" : "false");
#endif
    if (!guest_base || preparation_failed) {
        if (guest_base)
            translation_linux_abort_setup();
        translation_kernel_release(&loaded);
        if (!TRANSLATION_LOOKUP)
            free(views);
        translation_memory_stop();
        puts("{\"result\":\"failed\",\"reason\":\"translation_code_preparation\"}");
        return 1;
    }
    set_layout(guest_base, TRANSLATION_LOOKUP ? NCE_MM_TRANSLATED : NCE_MM_DEMAND, views, 3, NULL, 1, 0);
    const char *workload = getenv("NCE_TRANSLATION_WORKLOAD");
    REQUIRE(!workload || !strcmp(workload, "core") || !strcmp(workload, "bionic") || !strcmp(workload, "stock-art") || !strcmp(workload, "android") || !strcmp(workload, "services") || !strcmp(workload, "desktop"));
    int stock_bionic = workload && !strcmp(workload, "bionic");
    int stock_art = workload && !strcmp(workload, "stock-art");
    int desktop = workload && !strcmp(workload, "desktop");
    int services = desktop || (workload && !strcmp(workload, "services"));
    int android = services || (workload && !strcmp(workload, "android"));
    if (android) {
        REQUIRE(getenv("NCE_TRANSLATION_SYSTEM"));
        REQUIRE(nce_disk_open(1, getenv("NCE_TRANSLATION_SYSTEM")) == 0);
        if (services) {
            REQUIRE(getenv("NCE_TRANSLATION_DATA"));
            REQUIRE(nce_disk_open(0, getenv("NCE_TRANSLATION_DATA")) == 0);
        }
    }
    const char *online_text = getenv("NCE_TRANSLATION_ONLINE_CPUS");
    unsigned online_cpus = NCE_TRANSLATION_CPUS;
    if (online_text) {
        REQUIRE(!strcmp(online_text, "1") || !strcmp(online_text, "2"));
        online_cpus = (unsigned)(online_text[0] - '0');
    }
    REQUIRE(online_cpus <= NCE_TRANSLATION_CPUS);
    REQUIRE(init(&nce_lkl_host) == 0);
    unsigned long slice;
#if TRANSLATION_LOOKUP
    REQUIRE(start(scheduler_slice_us * 1000, &slice, online_cpus) == 0);
#else
    REQUIRE(start(scheduler_slice_us * 1000, &slice) == 0);
#endif
    REQUIRE(slice && (!scheduler_slice_us || slice == scheduler_slice_us * 1000));
    printf("{\"config\":\"linux_scheduler_slice\",\"requested_us\":%lu,\"applied_ns\":%lu}\n", scheduler_slice_us, slice);
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
    /* The host syscall task is pinned to CPU 0. /proc/stat reports the online
     * CPU set independently of that task's scheduling affinity. */
    long stat_args[6] = {-100, (long)"/proc/stat", 0};
    long stat_fd = call(56, stat_args);
    REQUIRE(stat_fd >= 0);
    char stat_text[1024] = {0};
    stat_args[0] = stat_fd;
    stat_args[1] = (long)stat_text;
    stat_args[2] = sizeof(stat_text) - 1;
    REQUIRE(call(63, stat_args) > 0 && call(57, stat_args) == 0);
    unsigned long online_mask = (strstr(stat_text, "\ncpu0 ") ? 1UL : 0UL) | (strstr(stat_text, "\ncpu1 ") ? 2UL : 0UL);
    REQUIRE(online_mask == (1UL << online_cpus) - 1);
    printf("{\"config\":\"translation_online_cpus\",\"capacity\":%u,\"requested\":%u,\"online_mask\":%lu}\n",
           NCE_TRANSLATION_CPUS, online_cpus, online_mask);
    put_file("/lib/nce-sigreturn.so", "sigreturn.elf");
    put_file("/probe-guest", "guest.elf");
    put_file("/probe-after-build", "late.elf");
    put_file("/dynamic", "dynamic.elf");
    put_file("/bin/sh", "dash.elf");
    if (!stock_art)
        put_file("/bionic-test", "bionic-test.elf");
    put_file("/lib/ld-musl-aarch64.so.1", "libc.so");
    put_file("/lib/dependency.so", "dependency.so");
    put_file("/lib/plugin.so", "plugin.so");
    unsigned long host_value = 0x7d2a9314UL;
    void *host_sources[2] = {0};
    if (TRANSLATION_LOOKUP && !android && !stock_bionic && !stock_art) {
        const char *fixtures[] = {"static", "static-2m", "pie", "pie-256k", "pie-2m", "pie-nonzero", "exec", "exec-near-end"};
        for (unsigned i = 0; i < sizeof(fixtures) / sizeof(*fixtures); ++i) {
            char guest[64], filename[64];
            REQUIRE(snprintf(guest, sizeof(guest), "/exec-aslr-%s", fixtures[i]) > 0);
            REQUIRE(snprintf(filename, sizeof(filename), "exec-aslr-%s.elf", fixtures[i]) > 0);
            put_file(guest, filename);
        }
        char text[256];
        const char *layout = getenv("NCE_TRANSLATION_GUEST_LAYOUT");
        REQUIRE(snprintf(text, sizeof(text), "%s %lu %lu %lu %lu %lu\n", layout ? layout : "host",
                         guest_base, state.guest_end, state.physical_base, state.ram, (unsigned long)&host_value) < (int)sizeof(text));
        put_text("/probe-layout", text);
        long pointer_args[6] = {-100, (long)"/host-pointer", 0x242, 0600};
        long pointer_fd = call(56, pointer_args);
        REQUIRE(pointer_fd >= 0);
        pointer_args[0] = pointer_fd;
        pointer_args[1] = (long)&host_value;
        pointer_args[2] = sizeof(host_value);
        REQUIRE(call(64, pointer_args) == (long)sizeof(host_value));
        unsigned long copied = 0;
        pointer_args[1] = (long)&copied;
        pointer_args[3] = 0;
        REQUIRE(call(67, pointer_args) == (long)sizeof(copied) && copied == host_value && call(57, pointer_args) == 0);
        if (layout && !strcmp(layout, "sparse")) {
            for (unsigned i = 0; i < 2; ++i) {
                vm_address_t high = state.guest_end;
                if (i)
                    REQUIRE(vm_map(mach_task_self(), &high, NCE_HOST_PAGE_SIZE, 0, VM_FLAGS_ANYWHERE,
                                   MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_DEFAULT) == KERN_SUCCESS);
                host_sources[i] = i ? (void *)high : host_low_source;
                unsigned long address = (unsigned long)host_sources[i];
                REQUIRE(i ? address >= state.guest_end : address >= guest_base && address <= state.guest_end - NCE_PAGE_SIZE);
                REQUIRE(snprintf(host_sources[i], NCE_PAGE_SIZE, "/host-source-%u", i) > 0);
                memcpy(host_sources[i] + 128, &host_value, sizeof(host_value));
                long args[6] = {-100, (long)host_sources[i], 0x242, 0600};
                long fd = call(56, args);
                REQUIRE(fd >= 0);
                args[0] = fd;
                args[1] = (long)host_sources[i] + 128;
                args[2] = sizeof(host_value);
                REQUIRE(call(64, args) == (long)sizeof(host_value));
                args[1] = (long)host_sources[i] + 256;
                args[3] = 0;
                REQUIRE(call(67, args) == (long)sizeof(host_value) && call(57, args) == 0);
                REQUIRE(!memcmp(host_sources[i] + 256, &host_value, sizeof(host_value)));
            }
            REQUIRE(snprintf(text, sizeof(text), "%lu %lu\n", (unsigned long)host_sources[0], (unsigned long)host_sources[1]) < (int)sizeof(text));
            put_text("/probe-host-sources", text);
            printf("{\"config\":\"translation_host_pointer_sources\",\"low\":%lu,\"high\":%lu,\"stack\":%lu,\"text\":%lu}\n",
                   (unsigned long)host_sources[0], (unsigned long)host_sources[1], (unsigned long)&host_value, (unsigned long)&nce_lkl_run);
        }
    }
    int failed = 0;
    if (stock_art) {
        stock_art_setup();
        translation_memory_phase("guest_execution");
        failed = run_program("/probe-guest", 1);
        stock_art_export();
    } else if (stock_bionic) {
        stock_bionic_setup();
        translation_memory_phase("guest_execution");
        failed = run_program("/probe-guest", 1);
    } else if (android) {
        android_setup(services, desktop);
        const char *programs[] = {"/bionic-test", "/system/bin/cfi-test", "/system/bin/art-test"};
        const char *phases[] = {"bionic_execution", "cfi_execution", "art_execution"};
        if (services) {
#ifdef NCE_GRAPHICS
            if (desktop) {
                REQUIRE(nce_graphics_start(call) == 0);
                nce_host_input_start(call);
                nce_session_set_syscall(call);
            }
#else
            REQUIRE(!desktop);
#endif
            translation_memory_phase(desktop ? "desktop_execution" : "services_execution");
            failed = run_program("/system/bin/android-services-test", 1);
#ifdef NCE_GRAPHICS
            if (desktop) {
                nce_session_set_syscall(NULL);
                nce_host_input_stop();
                nce_graphics_stop();
            }
#endif
        } else {
            for (unsigned i = 0; i < 3 && !failed; ++i) {
                translation_memory_phase(phases[i]);
                failed = run_program(programs[i], 0);
            }
        }
    } else {
        directory("/dev");
        long random_device[6] = {-100, (long)"/dev/random", 0020666, 264};
        REQUIRE(call(33, random_device) == 0);
        seed_entropy();
        translation_memory_phase("guest_execution");
        failed = run_program("/probe-guest", 0);
    }
    if (TRANSLATION_LOOKUP && !android && !stock_bionic && !stock_art) {
        REQUIRE(host_value == 0x7d2a9314UL);
        for (unsigned i = 0; i < 2; ++i) {
            if (host_sources[i]) {
                char expected[32];
                REQUIRE(snprintf(expected, sizeof(expected), "/host-source-%u", i) > 0);
                REQUIRE(!strcmp(host_sources[i], expected));
                REQUIRE(!memcmp(host_sources[i] + 128, &host_value, sizeof(host_value)));
                REQUIRE(!memcmp(host_sources[i] + 256, &host_value, sizeof(host_value)));
                if (i)
                    REQUIRE(!munmap(host_sources[i], NCE_HOST_PAGE_SIZE));
            }
        }
        puts("{\"test\":\"translated_host_pointer_copy\",\"passed\":true}");
    }
    memory_sample("guest_finished");
    translation_memory_phase("cleanup");
    if (services) {
        // Complete ext4 writes while the Linux worker threads are still running.
        long readonly[6] = {0, (long)"/data", 0, 1 | 32};
        REQUIRE(call(40, readonly) == 0);
        long temporary[6] = {(long)"/data/nce", 0};
        REQUIRE(call(39, temporary) == 0);
        long unmount[6] = {(long)"/data", 0};
        REQUIRE(call(39, unmount) == 0);
        puts("{\"test\":\"linux_data_readonly_shutdown\",\"passed\":true}");
    }
    REQUIRE(halt() == 0);
    cleanup();
    int disk_close_result = nce_disk_close();
    failed |= disk_close_result != 0;
    translation_linux_cleanup();
    REQUIRE(translation_kernel_release(&loaded) == 0);
    if (!TRANSLATION_LOOKUP)
        free(views);
    memory_sample("cleaned");
    translation_memory_stop();
    puts(failed ? "{\"result\":\"failed\"}" : "{\"result\":\"passed\"}");
    return failed;
}

#if !defined(TRANSLATION_DISPATCH) && !defined(NCE_IOS_APP)
int main(int argc, char **argv)
{
    return argc == 2 ? nce_lkl_run(argv[1]) : 2;
}
#endif
