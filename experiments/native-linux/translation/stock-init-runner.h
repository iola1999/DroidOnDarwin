/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <time.h>

static int stock_init_raw_path(const char *path, int hyperos)
{
    const char *partitions[] = {"/system", "/product", "/system_ext", "/vendor", "/system_dlkm"};
    for (unsigned i = 0; i < (hyperos ? 5U : 1U); ++i) {
        size_t length = strlen(partitions[i]);
        if (!strcmp(path, partitions[i]) || (!strncmp(path, partitions[i], length) && path[length] == '/'))
            return 1;
    }
    return 0;
}

static void stock_init_dump_kernel_log(void)
{
    char *kernel_log = calloc(1, 1 << 20);
    REQUIRE(kernel_log);
    long log_args[6] = {3, (long)kernel_log, (1 << 20) - 1};
    long log_bytes = call(116, log_args);
    if (log_bytes > 0)
        REQUIRE(fwrite(kernel_log, 1, log_bytes, stderr) == (size_t)log_bytes);
    free(kernel_log);
    printf("{\"stock_init_kernel_log\":true,\"read_result\":%ld}\n", log_bytes);
}

static int stock_init_dump_output(long fd, FILE *output)
{
    unsigned char buffer[65536];
    unsigned long offset = 0;
    long result;
    for (;;) {
        long args[6] = {fd, (long)buffer, sizeof(buffer), (long)offset};
        result = call(67, args);
        if (result == -4)
            continue;
        if (result <= 0)
            break;
        if (fwrite(buffer, 1, result, output) != (size_t)result) {
            result = -5;
            break;
        }
        offset += result;
    }
    printf("{\"stock_init_output\":true,\"fd\":%ld,\"bytes\":%lu,\"read_result\":%ld}\n", fd, offset, result);
    return result != 0;
}

static void stock_init_remove_bootstrap(void)
{
    /* LKL 的空 /init 用于启动宿主 syscall 任务，原根内容在此之后恢复。 */
    long bootstrap[6] = {-100, (long)"/init", 0x20000};
    long fd = call(56, bootstrap);
    REQUIRE(fd >= 0);
    /* Linux statx 使用固定的 256-byte ABI，此处仅读取所需前缀。 */
    struct {
        uint32_t stx_mask, stx_blksize;
        uint64_t stx_attributes;
        uint32_t stx_nlink, stx_uid, stx_gid;
        uint16_t stx_mode, spare;
        uint64_t stx_ino, stx_size;
        unsigned char remainder[208];
    } status = {0};
    _Static_assert(sizeof(status) == 256, "Linux statx buffer size");
    long info[6] = {fd, (long)"", 0x1000, 0x21f, (long)&status};
    REQUIRE(call(291, info) == 0 && (status.stx_mask & 0x21f) == 0x21f);
    fprintf(stderr, "stock_init_bootstrap path=/init uid=%u gid=%u mode=%u links=%u bytes=%llu\n", status.stx_uid, status.stx_gid, status.stx_mode, status.stx_nlink, (unsigned long long)status.stx_size);
    REQUIRE(status.stx_mode == 0100700 && status.stx_uid == 0 && status.stx_gid == 0 && status.stx_nlink == 1 && status.stx_size == 0);
    unsigned char byte;
    bootstrap[0] = fd;
    bootstrap[1] = (long)&byte;
    bootstrap[2] = 1;
    REQUIRE(call(63, bootstrap) == 0 && call(57, bootstrap) == 0);
    bootstrap[0] = -100;
    bootstrap[1] = (long)"/init";
    bootstrap[2] = 0;
    REQUIRE(call(35, bootstrap) == 0);
}

static double stock_init_now(void);
#include "stock-init-partitions.h"
#include "stock-init-data.h"
#include "stock-init-metadata.h"
#include "stock-init-stop.h"
#if defined(NCE_STOCK_INIT_RAMDISK)
#include "stock-init-first-stage-dm.h"
#include "stock-init-first-stage-devices.h"
#include "stock-init-ramdisk.h"
#endif

static int stock_init_first_stage_enabled(void)
{
#if defined(NCE_STOCK_INIT_RAMDISK)
    const char *value = getenv("NCE_STOCK_INIT_FIRST_STAGE");
    REQUIRE(value && (!strcmp(value, "0") || !strcmp(value, "1")));
    return !strcmp(value, "1");
#else
    return 0;
#endif
}

static void stock_init_setup(void)
{
    const char *root = getenv("NCE_STOCK_INIT_DIRECTORY");
    REQUIRE(root && root[0] == '/');
#if defined(NCE_STOCK_INIT_RAMDISK)
    if (stock_init_first_stage_enabled()) {
        REQUIRE(stock_init_first_stage_dm_prepare() == 0);
        stock_init_first_stage_dm_parameter_boundaries();
        stock_init_remove_bootstrap();
        stock_init_ramdisk_setup();
        long sysfs[6] = {(long)"sysfs", (long)"/sys", (long)"sysfs", 2 | 4 | 8};
        long mounted = call(40, sysfs);
        printf("{\"stock_init_first_stage_sysfs_mount\":true,\"operation\":\"mount\",\"result\":%ld}\n", mounted);
        REQUIRE(mounted == 0);
        const char *data = getenv("NCE_TRANSLATION_DATA");
        const char *metadata = getenv("NCE_STOCK_INIT_METADATA");
        long devices = stock_init_first_stage_devices(1, data && data[0], metadata && metadata[0]);
        long unmount[6] = {(long)"/sys", 0};
        long unmounted = call(39, unmount);
        printf("{\"stock_init_first_stage_sysfs_mount\":true,\"operation\":\"unmount\",\"result\":%ld}\n", unmounted);
        REQUIRE(unmounted == 0 && devices == 0);
        puts("{\"stock_init_ramdisk_ready\":true,\"entries\":29,\"proc_restored\":true}");
        put_text("/stock-init-profile", "sdk36-first-stage");
        const char *mode = getenv("NCE_STOCK_INIT_MODE");
        REQUIRE(mode && (!strcmp(mode, "lifecycle") || !strcmp(mode, "original")));
        put_text("/stock-init-mode", mode);
        return;
    }
#endif
    const char *original_image = getenv("NCE_STOCK_INIT_SYSTEM");
    int original_system = original_image && original_image[0];
    const char *vendor_image = getenv("NCE_STOCK_INIT_VENDOR");
    int hyperos = vendor_image && vendor_image[0];
    const char *lp_text = getenv("NCE_STOCK_INIT_LP");
    int lp = lp_text && !strcmp(lp_text, "1");
    int complete = hyperos || lp;
    REQUIRE(!lp || (original_system && !hyperos));
    REQUIRE(!hyperos || (original_system && getenv("NCE_STOCK_INIT_SYSTEM_DLKM") && getenv("NCE_STOCK_INIT_SYSTEM_DLKM")[0]));
    /* Original first-stage init uses tmpfs so nodes are accessible before restorecon. */
    directory("/dev");
    long devfs[6] = {(long)"tmpfs", (long)"/dev", (long)"tmpfs", 2, (long)"mode=0755"};
    REQUIRE(call(40, devfs) == 0);
    if (original_system) {
        directory("/sys");
        directory("/system-image");
        directory("/system");
        long sysfs[6] = {(long)"sysfs", (long)"/sys", (long)"sysfs"};
        long mounted = call(40, sysfs);
#if defined(NCE_STOCK_INIT_RAMDISK)
        printf("{\"stock_init_first_stage_sysfs_mount\":true,\"operation\":\"mount\",\"result\":%ld}\n", mounted);
#endif
        REQUIRE(mounted == 0);
#if defined(NCE_STOCK_INIT_RAMDISK)
        REQUIRE(lp && !hyperos);
        const char *data = getenv("NCE_TRANSLATION_DATA");
        const char *metadata = getenv("NCE_STOCK_INIT_METADATA");
        REQUIRE(stock_init_first_stage_devices(0, data && data[0], metadata && metadata[0]) == 0);
#endif
        block_device("/sys/block/ncedisk1/dev", "/dev/ncesystem", 0400);
        if (lp)
            stock_init_sdk_mount(root);
        else {
            long partition[6] = {(long)"/dev/ncesystem", (long)"/system-image", (long)"ext4", 1};
            REQUIRE(call(40, partition) == 0);
        }
        long system[6] = {(long)"/system-image/system", (long)"/system", 0, 4096};
        REQUIRE(call(40, system) == 0);
        puts("{\"stock_init_original_system_mounted\":true,\"readonly\":true}");
        if (hyperos) {
            const char *bound[] = {"/product", "/system_ext"};
            for (unsigned i = 0; i < sizeof(bound) / sizeof(*bound); ++i) {
                char source[64];
                REQUIRE(snprintf(source, sizeof(source), "/system-image%s", bound[i]) < (int)sizeof(source));
                directory(bound[i]);
                long bind[6] = {(long)source, (long)bound[i], 0, 4096};
                REQUIRE(call(40, bind) == 0);
                printf("{\"stock_init_original_partition_mounted\":true,\"path\":\"%s\",\"filesystem\":\"bind\",\"readonly\":true}\n", bound[i]);
            }
            const char *erofs[] = {"/vendor", "/system_dlkm"};
            for (unsigned i = 0; i < sizeof(erofs) / sizeof(*erofs); ++i) {
                char sysfs_path[64], device[64];
                REQUIRE(snprintf(sysfs_path, sizeof(sysfs_path), "/sys/block/ncedisk%u/dev", i + 2) < (int)sizeof(sysfs_path));
                REQUIRE(snprintf(device, sizeof(device), "/dev/nce%s", erofs[i] + 1) < (int)sizeof(device));
                block_device(sysfs_path, device, 0400);
                directory(erofs[i]);
                long mount[6] = {(long)device, (long)erofs[i], (long)"erofs", 1};
                long mounted = call(40, mount);
                if (mounted) {
                    fprintf(stderr, "stock_init_mount path=%s filesystem=erofs result=%ld\n", erofs[i], mounted);
                    stock_init_dump_kernel_log();
                }
                REQUIRE(mounted == 0);
                printf("{\"stock_init_original_partition_mounted\":true,\"path\":\"%s\",\"filesystem\":\"erofs\",\"readonly\":true}\n", erofs[i]);
            }
        }
    }
    if (complete)
        stock_init_remove_bootstrap();
    char path[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/files.tsv", root) < (int)sizeof(path));
    FILE *input = fopen(path, "r");
    REQUIRE(input);
    char line[2048];
    while (fgets(line, sizeof(line), input)) {
        char guest[1024], extra;
        unsigned mode;
        REQUIRE(strchr(line, '\n'));
        if (line[0] == 'D') {
            REQUIRE(sscanf(line, "D\t%1023s %c", guest, &extra) == 1);
            if (!original_system || !stock_init_raw_path(guest, complete))
                directory(guest);
        } else if (line[0] == 'L') {
            char target[1024], actual[1024] = {0};
            char label[256] = {0};
            unsigned uid, gid;
            if (complete)
                REQUIRE(sscanf(line, "L\t%u\t%u\t%1023s\t%1023s\t%255s %c", &uid, &gid, guest, target, label, &extra) == 5);
            else
                REQUIRE(sscanf(line, "L\t%u\t%u\t%1023s\t%1023s %c", &uid, &gid, guest, target, &extra) == 4);
            REQUIRE(guest[0] == '/' && target[0] == '/' && !strstr(guest, "..") && !strstr(target, ".."));
            long link[6] = {(long)target, -100, (long)guest};
            REQUIRE(call(36, link) == 0);
            long owner[6] = {-100, (long)guest, uid, gid, 0x100};
            REQUIRE(call(54, owner) == 0);
            if (complete) {
                long attribute[6] = {(long)guest, (long)"security.selinux", (long)label, (long)strlen(label) + 1, 0};
                long assigned = call(6, attribute);
                if (assigned)
                    fprintf(stderr, "stock_init_root_label path=%s result=%ld\n", guest, assigned);
                REQUIRE(assigned == 0);
            }
            long readlink[6] = {-100, (long)guest, (long)actual, sizeof(actual) - 1};
            long length = call(78, readlink);
            REQUIRE(length == (long)strlen(target) && !memcmp(actual, target, length));
            printf("{\"stock_init_root_symlink\":true,\"path\":\"%s\",\"target\":\"%s\",\"uid\":%u,\"gid\":%u,\"creation_mode\":511}\n", guest, target, uid, gid);
        } else {
            REQUIRE(sscanf(line, "F\t%o\t%1023s %c", &mode, guest, &extra) == 2);
            REQUIRE(guest[0] == '/' && !strstr(guest, ".."));
            REQUIRE(snprintf(path, sizeof(path), "%s/root%s", root, guest) < (int)sizeof(path));
            if (!original_system || !stock_init_raw_path(guest, complete))
                put_host_file(guest, path, mode);
        }
    }
    REQUIRE(!ferror(input) && fclose(input) == 0);
    if (complete) {
        long root_file[6] = {(long)"/system-image/init.environ.rc", (long)"/init.environ.rc", 0, 4096};
        REQUIRE(call(40, root_file) == 0);
        put_file("/stock-init-original-metadata.tsv", "original-metadata.tsv");
        puts("{\"stock_init_original_root_file\":true,\"path\":\"/init.environ.rc\",\"readonly\":true}");
    }
    put_text("/stock-init-profile", hyperos ? "hyperos" : lp ? "sdk36-lp"
                                                             : "sdk36");
    const char *directories[] = {"/dev", "/dev/socket", "/dev/block", "/dev/pts", "/dev/dm-user", "/sys", "/apex", "/bootstrap-apex", "/linkerconfig", "/mnt", "/data", "/metadata", "/debug_ramdisk", "/second_stage_resources"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        directory(directories[i]);
    const char *devices[] = {"/dev/null", "/dev/random", "/dev/urandom", "/dev/kmsg", "/dev/ptmx"};
    const unsigned numbers[] = {259, 264, 265, 267, 1282};
    for (unsigned i = 0; i < sizeof(devices) / sizeof(*devices); ++i) {
        long args[6] = {-100, (long)devices[i], 0020000 | (i == 3 ? 0600 : 0666), numbers[i]};
        REQUIRE(call(33, args) == 0);
    }
    seed_entropy();
    long console[6] = {8, 0, 8};
    REQUIRE(call(116, console) == 0);
    const char *filesystems[][2] = {{"sysfs", "/sys"}, {"selinuxfs", "/sys/fs/selinux"}, {"devpts", "/dev/pts"}, {"tmpfs", "/apex"}, {"tmpfs", "/bootstrap-apex"}, {"tmpfs", "/linkerconfig"}};
    for (unsigned i = 0; i < sizeof(filesystems) / sizeof(*filesystems); ++i) {
        if (original_system && !strcmp(filesystems[i][0], "sysfs"))
            continue;
        long args[6] = {(long)filesystems[i][0], (long)filesystems[i][1], (long)filesystems[i][0]};
        REQUIRE(call(40, args) == 0);
    }
    const char *partitions[] = {"/system", "/vendor", "/product", "/system_ext"};
    for (unsigned i = 0; i < sizeof(partitions) / sizeof(*partitions); ++i) {
        long args[6] = {(long)partitions[i], (long)partitions[i], 0, 4096};
        REQUIRE(call(40, args) == 0);
    }
    const char *mode = getenv("NCE_STOCK_INIT_MODE");
    REQUIRE(mode && (!strcmp(mode, "lifecycle") || !strcmp(mode, "original")));
    put_text("/stock-init-mode", mode);
    stock_init_data_mount();
    stock_init_metadata_mount();
}

static double stock_init_now(void)
{
    struct timespec now;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void stock_init_observe_context(long pid, char *previous, long *previous_result)
{
    char path[64], label[256] = {0};
    REQUIRE(snprintf(path, sizeof(path), "/proc/%ld/attr/current", pid) < (int)sizeof(path));
    long args[6] = {-100, (long)path, 0};
    long result = call(56, args);
    if (result >= 0) {
        args[0] = result;
        args[1] = (long)label;
        args[2] = sizeof(label) - 1;
        result = call(63, args);
        REQUIRE(call(57, args) == 0);
    }
    if (result == *previous_result && !strcmp(label, previous))
        return;
    *previous_result = result;
    memcpy(previous, label, sizeof(label));
    flockfile(stdout);
    printf("{\"stock_init_task_context\":true,\"ancestor_pid\":%ld,\"read_result\":%ld,\"label_hex\":\"", pid, result);
    for (long i = 0; i < result && i < (long)sizeof(label) - 1; ++i)
        printf("%02x", (unsigned char)label[i]);
    puts("\"}");
    funlockfile(stdout);
}

static int stock_init_run(void)
{
    int first_stage = stock_init_first_stage_enabled();
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
    if (!first_stage) {
        stock_init_volume_retain(&stock_init_data);
        stock_init_volume_retain(&stock_init_metadata);
    }
    long launch[6] = {(long)translation_linux_process("/probe-guest"), 1};
    long pid = call(246, launch);
    REQUIRE(pid > 1);
#if defined(NCE_STOCK_INIT_RAMDISK)
    if (first_stage) {
        long dm_args[6] = {0};
        long result = call(248, dm_args);
        printf("{\"stock_init_first_stage_dm_host_boundary\":true,\"operation\":\"running_namespace\",\"result\":%ld,\"ancestor_pid\":%ld}\n", result, pid);
        REQUIRE(result == -16);
    }
#endif
    int status = -1, stopped = 0;
    long waited = 0, killed = 0;
    int lifecycle = !strcmp(getenv("NCE_STOCK_INIT_MODE"), "lifecycle");
    double deadline = stock_init_now() + (lifecycle ? 2 : 30);
    char previous_context[256] = {0};
    long previous_result = -1;
    for (;;) {
        long wait[6] = {pid, (long)&status, 1};
        waited = call(260, wait);
        REQUIRE(waited == 0 || waited == pid);
        if (waited == pid)
            break;
        if (!lifecycle)
            stock_init_observe_context(pid, previous_context, &previous_result);
        if (stock_init_now() >= deadline) {
            if (lifecycle)
                stock_init_stop_boundaries(pid);
            waited = stock_init_stop(pid, &status, &killed, &stopped);
            break;
        }
        struct timespec delay = {0, 10000000};
        REQUIRE(nanosleep(&delay, NULL) == 0);
    }
    int output_failed = 0;
    if (!first_stage) {
        output_failed = stock_init_dump_output(1, stdout);
        output_failed |= stock_init_dump_output(2, stderr);
        stock_init_dump_kernel_log();
    }
    printf("{\"stock_init_observation\":true,\"mode\":\"%s\",\"ancestor_pid\":%ld,\"waited\":%ld,\"status\":%d,\"deadline_stop\":%s,\"kill_result\":%ld}\n", getenv("NCE_STOCK_INIT_MODE"), pid, waited, status, stopped ? "true" : "false", killed);
    long reaped[6] = {pid};
    long reaped_result = call(247, reaped);
    printf("{\"stock_init_host_stop_reaped\":true,\"ancestor_pid\":%ld,\"result\":%ld}\n", pid, reaped_result);
    REQUIRE(reaped_result == -3);
#if defined(NCE_STOCK_INIT_RAMDISK)
    if (first_stage) {
        long sync_args[6] = {0};
        long synced = call(81, sync_args);
        printf("{\"stock_init_first_stage_sync\":true,\"result\":%ld}\n", synced);
        long dm_result = stock_init_first_stage_dm_cleanup();
        output_failed = stock_init_dump_output(1, stdout);
        output_failed |= stock_init_dump_output(2, stderr);
        stock_init_dump_kernel_log();
        return synced != 0 || dm_result != 0 || output_failed || (lifecycle && (!stopped || status != 9));
    }
#endif
    int metadata_failed = stock_init_metadata_finish(lifecycle);
    int data_failed = stock_init_volume_cleanup(&stock_init_data, lifecycle);
    if (getenv("NCE_STOCK_INIT_LP") && !strcmp(getenv("NCE_STOCK_INIT_LP"), "1"))
        stock_init_sdk_cleanup(lifecycle);
    return metadata_failed || data_failed || output_failed || (lifecycle && (!stopped || status != 9));
}
