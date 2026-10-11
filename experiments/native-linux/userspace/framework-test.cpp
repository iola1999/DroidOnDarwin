/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "diagnostics.h"
#include <errno.h>
#include "desktop-session.h"
#include <android/binder_manager.h>
#include <android/binder_ibinder.h>
#include <dlfcn.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <initializer_list>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "framework-test:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(109); \
        } \
    } while (0)

static void runtime_environment()
{
    const char *environment[][2] = {
        {"ANDROID_ROOT", "/system"},
        {"ANDROID_DATA", "/data"},
        {"ANDROID_ART_ROOT", "/apex/com.android.art"},
        {"ANDROID_I18N_ROOT", "/apex/com.android.i18n"},
        {"ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata"},
        {"ICU_DATA", "/apex/com.android.i18n/etc/icu"},
        {"CLASSPATH", "/system/framework/nce-framework.jar"},
        {"LD_PRELOAD", "/system/lib64/libnce-framework-log.so"},
    };
    for (const auto &entry : environment)
        CHECK(setenv(entry[0], entry[1], 1) == 0);
    if (access("/data/nce/graphics", F_OK) == 0) {
        CHECK(setenv("LD_PRELOAD", "/system/lib64/libnce-qemupipe.so:/system/lib64/libnce-framework-log.so", 1) == 0);
        CHECK(setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/vendor/lib64/egl:/vendor/lib64/hw", 1) == 0);
    }
}

void nce_framework_check(const char *name, unsigned seconds)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(seconds);
        runtime_environment();
        execl("/system/bin/app_process64", "app_process64", "-Djava.library.path=/system/lib64",
              "/system/bin", name, nullptr);
        _exit(108);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    if (status)
        dprintf(2, "framework check %s child=%d status=%d\n", name, child, status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void derive_classpath()
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        execl("/system/bin/derive_classpath", "derive_classpath", "/data/nce/classpath", nullptr);
        _exit(108);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    FILE *file = fopen("/data/nce/classpath", "r");
    CHECK(file);
    char line[16384];
    unsigned variables = 0;
    while (fgets(line, sizeof(line), file)) {
        CHECK(!strncmp(line, "export ", 7) && strchr(line, '\n'));
        line[strcspn(line, "\n")] = 0;
        char *value = strchr(line + 7, ' ');
        CHECK(value);
        *value++ = 0;
        const char *key = line + 7;
        CHECK(!strcmp(key, "BOOTCLASSPATH") || !strcmp(key, "DEX2OATBOOTCLASSPATH") ||
              !strcmp(key, "SYSTEMSERVERCLASSPATH") || !strcmp(key, "STANDALONE_SYSTEMSERVER_JARS"));
        CHECK(setenv(key, value, 1) == 0);
        for (char *path = strtok(value, ":"); path; path = strtok(nullptr, ":"))
            CHECK(access(path, R_OK) == 0);
        ++variables;
    }
    CHECK(!ferror(file) && fclose(file) == 0 && variables == 4);
    CHECK(getenv("BOOTCLASSPATH") && getenv("SYSTEMSERVERCLASSPATH"));
    puts("{\"test\":\"android_derived_classpaths\",\"passed\":true,\"variables\":4}");
}

static void compare_files(const char *source, const char *generated)
{
    int first = open(source, O_RDONLY | O_CLOEXEC);
    int second = open(generated, O_RDONLY | O_CLOEXEC);
    if (first < 0 || second < 0)
        dprintf(2, "aconfig files: %s -> %s\n", source, generated);
    CHECK(first >= 0 && second >= 0);
    char left[4096], right[4096];
    for (;;) {
        ssize_t count = read(first, left, sizeof(left));
        CHECK(count >= 0 && read(second, right, sizeof(right)) == count);
        CHECK(!memcmp(left, right, count));
        if (!count)
            break;
    }
    CHECK(close(first) == 0 && close(second) == 0);
}

static bool check_aconfig_container(const char *container, const char *source)
{
    char from[512], to[512];
    struct stat info;
    snprintf(from, sizeof(from), "%s/flag.val", source);
    if (stat(from, &info) || !info.st_size)
        return false;
    const char *files[][2] = {{"package.map", "maps/%s.package.map"},
                              {"flag.map", "maps/%s.flag.map"},
                              {"flag.val", "boot/%s.val"},
                              {"flag.info", "boot/%s.info"}};
    for (const auto &file : files) {
        snprintf(from, sizeof(from), "%s/%s", source, file[0]);
        char relative[256];
        snprintf(relative, sizeof(relative), file[1], container);
        snprintf(to, sizeof(to), "/metadata/aconfig/%s", relative);
        compare_files(from, to);
    }
    return true;
}

static void initialize_aconfig()
{
    for (const char *path : {"/metadata", "/metadata/aconfig", "/metadata/aconfig/flags",
                             "/metadata/aconfig/maps", "/metadata/aconfig/boot"}) {
        CHECK(mkdir(path, 0775) == 0 || errno == EEXIST);
        CHECK(chown(path, 0, 1000) == 0 && chmod(path, 0775) == 0);
    }
    unsigned containers = 0;
    for (unsigned round = 0; round < 2; ++round) {
        const char *commands[][2] = {
            {"/system/bin/aconfigd-system", "platform-init"},
            {"/apex/com.android.configinfrastructure/bin/aconfigd-mainline", "bootstrap-init"},
            {"/apex/com.android.configinfrastructure/bin/aconfigd-mainline", "init"},
        };
        for (const auto &command : commands) {
            pid_t child = fork();
            CHECK(child >= 0);
            if (!child) {
                CHECK(setenv("LD_PRELOAD", "/system/lib64/libnce-framework-log.so", 1) == 0);
                CHECK(setgid(1000) == 0 && setuid(1000) == 0);
                execl(command[0], command[0], command[1], nullptr);
                _exit(108);
            }
            int status;
            CHECK(waitpid(child, &status, 0) == child);
            if (status)
                dprintf(2, "aconfigd %s %s status=%d\n", command[0], command[1], status);
            CHECK(status == 0);
        }
        containers = 0;
        for (const char *name : {"system", "system_ext", "product", "vendor"}) {
            char source[128];
            snprintf(source, sizeof(source), "/%s/etc/aconfig", name);
            CHECK(check_aconfig_container(name, source));
            ++containers;
        }
        DIR *directory = opendir("/apex");
        CHECK(directory);
        while (dirent *entry = readdir(directory)) {
            if (strncmp(entry->d_name, "com.android.", 12))
                continue;
            char source[512];
            snprintf(source, sizeof(source), "/apex/%s/etc", entry->d_name);
            if (check_aconfig_container(entry->d_name, source))
                ++containers;
        }
        CHECK(closedir(directory) == 0 && containers > 1);
    }
    printf("{\"test\":\"android_aconfig_storage\",\"passed\":true,\"containers\":%u,\"initializations\":2}\n", containers);
}

void nce_framework_test()
{
    CHECK(symlink("/system/etc", "/etc") == 0);
    const char *properties[][2] = {
        {"ro.product.cpu.abilist64", "arm64-v8a"},
        {"ro.product.cpu.abilist", "arm64-v8a"},
        {"ro.product.cpu.abi", "arm64-v8a"},
        {"ro.build.version.sdk", "36"},
        {"ro.build.version.sdk_full", "36.0"},
        {"ro.build.version.release", "16"},
        {"ro.build.version.release_or_codename", "16"},
        {"ro.build.version.codename", "REL"},
        {"ro.build.version.all_codenames", "REL"},
        // KeyMint waits for these properties to exist. This modified image
        // does not claim a platform or vendor security patch level.
        {"ro.build.version.security_patch", ""},
        {"ro.vendor.build.security_patch", ""},
        {"ro.product.model", "NCE prototype"},
        {"ro.build.type", "userdebug"},
        // Blank data images and startup without a boot image need a longer
        // bounded service deadline during the current performance experiments.
        {"ro.hw_timeout_multiplier", "10"},
        {"ro.treble.enabled", "true"},
        {"sys.use_memfd", "true"},
        {"dalvik.vm.heapstartsize", "16m"},
        {"dalvik.vm.heapsize", "128m"},
        {"dalvik.vm.jdwp-provider", "none"},
        {"dalvik.vm.usejit", "true"},
        {"dalvik.vm.jitmaxsize", "32m"},
        {"dalvik.vm.jitthreshold", "1024"},
        {"dalvik.vm.gctype", "MS"},
        {"dalvik.vm.boot-image", "/data/no-boot-image.art"},
        {"dalvik.vm.extra-opts", "-Xnoimage-dex2oat -Xjitwarmupthreshold:1024"},
    };
    for (const auto &entry : properties)
        CHECK(__system_property_set(entry[0], entry[1]) == 0);
    FILE *boot_mode = fopen("/data/nce/art-boot-image", "r");
    CHECK(boot_mode);
    int boot_enabled = fgetc(boot_mode);
    CHECK(fclose(boot_mode) == 0 && (boot_enabled == '0' || boot_enabled == '1'));
    if (boot_enabled == '1') {
        CHECK(access("/system/framework/arm64/nce-boot.art", R_OK) == 0);
        CHECK(__system_property_set("dalvik.vm.boot-image", "/system/framework/nce-boot.art") == 0);
    }
    // APK execution uses the prepared ART JIT. Keep installation verification
    // separate from dex2oat code generation, whose output is not prepared yet.
    const char *dexopt_reasons[] = {"ab-ota", "bg-dexopt", "boot-after-mainline-update", "boot-after-ota",
                                    "cmdline", "first-boot", "inactive", "install", "install-bulk", "install-bulk-downgraded",
                                    "install-bulk-secondary", "install-bulk-secondary-downgraded", "install-fast", "post-boot", "shared"};
    for (const char *reason : dexopt_reasons) {
        char key[96];
        snprintf(key, sizeof(key), "pm.dexopt.%s", reason);
        CHECK(__system_property_set(key, "verify") == 0);
    }
    CHECK(__system_property_set("config.disable_otadexopt", "true") == 0);
    CHECK(__system_property_set("dalvik.vm.disable-art-service-dexopt", "true") == 0);
    CHECK(__system_property_set("pm.dexopt.disable_bg_dexopt", "true") == 0);
    initialize_aconfig();
    derive_classpath();
    if (nce_diagnostics())
        nce_framework_check("NceFramework");
}

static int control_socket(const char *name, int type = SOCK_STREAM, gid_t group = 1000,
                          uid_t owner = 0, mode_t mode = 0660)
{
    int fd = socket(AF_UNIX, type, 0);
    CHECK(fd >= 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    CHECK(snprintf(address.sun_path, sizeof(address.sun_path), "/dev/socket/%s", name) > 0);
    CHECK(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    CHECK(chown(address.sun_path, owner, group) == 0 && chmod(address.sun_path, mode) == 0);
    if (type == SOCK_SEQPACKET || type == SOCK_DGRAM) {
        int passcred = 1;
        CHECK(setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof(passcred)) == 0);
    }
    if (type != SOCK_DGRAM)
        CHECK(listen(fd, 8) == 0);
    return fd;
}

bool nce_surface_check(const char *package, const char *png, bool cancellable)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(210);
        runtime_environment();
        // DisplayControl belongs to the server JNI library. Keep its static
        // Binder descriptors alive until process exit, alongside Framework.
        CHECK(setenv("LD_PRELOAD", "/system/lib64/libnce-qemupipe.so:/system/lib64/libnce-framework-log.so:/system/lib64/libandroid_servers.so", 1) == 0);
        char classpath[16384];
        CHECK(snprintf(classpath, sizeof(classpath), "/system/framework/nce-framework.jar:%s", getenv("SYSTEMSERVERCLASSPATH")) < static_cast<int>(sizeof(classpath)));
        CHECK(setenv("CLASSPATH", classpath, 1) == 0);
        execl("/system/bin/app_process64", "app_process64", "-Djava.library.path=/system/lib64",
              "/system/bin", "NceSurface", package, png, nullptr);
        _exit(108);
    }
    int status = nce_wait_desktop_child(child, cancellable);
    if (status == -ECANCELED)
        return false;
    if (status)
        dprintf(2, "surface-test child=%d status=%d\n", child, status);
    CHECK(status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return true;
}

void nce_surface_test()
{
    CHECK(nce_surface_check());
}

static void receive_bytes(int fd, void *buffer, size_t size)
{
    while (size) {
        ssize_t count = read(fd, buffer, size);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count > 0);
        buffer = static_cast<char *>(buffer) + count;
        size -= count;
    }
}

static void wait_system_service(pid_t child, const char *name)
{
    for (unsigned i = 0; i < 100; ++i) {
        int status;
        pid_t exited = waitpid(child, &status, WNOHANG);
        if (exited == child)
            dprintf(2, "service %s pid=%d exited status=%d\n", name, child, status);
        CHECK(exited == 0);
        AIBinder *service = AServiceManager_checkService(name);
        if (service) {
            AIBinder_decStrong(service);
            return;
        }
        usleep(100000);
    }
    dprintf(2, "pid=%d did not publish %s\n", child, name);
    CHECK(false);
}

void nce_android_command(std::initializer_list<const char *> arguments,
                         const char *expected)
{
    int output = open("/data/nce/command-output", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(output >= 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        runtime_environment();
        CHECK(dup2(output, STDOUT_FILENO) == STDOUT_FILENO && close(output) == 0);
        char *argv[24] = {const_cast<char *>("cmd")};
        CHECK(arguments.size() < 23);
        unsigned index = 1;
        for (const char *argument : arguments)
            argv[index++] = const_cast<char *>(argument);
        execv("/system/bin/cmd", argv);
        _exit(108);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    char text[8192] = {};
    ssize_t count = pread(output, text, sizeof(text) - 1, 0);
    CHECK(count >= 0 && count < static_cast<ssize_t>(sizeof(text) - 1) && close(output) == 0);
    dprintf(2, "Android cmd");
    for (const char *argument : arguments)
        dprintf(2, " %s", argument);
    dprintf(2, " status=%d\n%s", status, text);
    CHECK(status == 0 && (!expected || strstr(text, expected)));
}

static void stop_process_group(pid_t leader)
{
    CHECK(leader > 1);
    CHECK(kill(-leader, SIGTERM) == 0 || errno == ESRCH);
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int status;
        pid_t child;
        while ((child = waitpid(-leader, &status, WNOHANG)) > 0) {
        }
        CHECK(child == 0 || errno == ECHILD);
        if (kill(-leader, 0) == -1) {
            CHECK(errno == ESRCH);
            return;
        }
        if (attempt == 50)
            CHECK(kill(-leader, SIGKILL) == 0 || errno == ESRCH);
        usleep(10000);
    }
    dprintf(2, "process group %d did not stop\n", leader);
    CHECK(false);
}

static void shutdown_framework()
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        alarm(30);
        runtime_environment();
        execl("/system/bin/app_process64", "app_process64", "/system/bin", "NceShutdown", nullptr);
        _exit(108);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    if (status)
        dprintf(2, "Android framework shutdown status=%d; continuing process cleanup and filesystem sync\n", status);
}

pid_t nce_start_system_service(const char *path, const char *name, uid_t uid,
                               std::initializer_list<const char *> arguments)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setpgid(0, 0) == 0);
        runtime_environment();
        CHECK(setgid(uid == 0 ? 0 : 1000) == 0 && setuid(uid) == 0);
        char *argv[16] = {const_cast<char *>(path)};
        CHECK(arguments.size() < 15);
        unsigned index = 1;
        for (const char *argument : arguments)
            argv[index++] = const_cast<char *>(argument);
        execv(path, argv);
        _exit(108);
    }
    if (name)
        wait_system_service(child, name);
    return child;
}

static pid_t start_socket_service(const char *path, const char *service,
                                  std::initializer_list<const char *> names, int type,
                                  uid_t uid = 0, mode_t mode = 0660)
{
    int descriptors[8];
    CHECK(names.size() <= 8);
    unsigned count = 0;
    for (const char *name : names)
        descriptors[count++] = control_socket(name, type,
                                              !strcmp(name, "dnsproxyd") || !strcmp(name, "fwmarkd") ? 3003 : (uid ? uid : 1000), uid, mode);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setpgid(0, 0) == 0);
        runtime_environment();
        unsigned i = 0;
        for (const char *name : names) {
            char key[64], value[24];
            snprintf(key, sizeof(key), "ANDROID_SOCKET_%s", name);
            snprintf(value, sizeof(value), "%d", descriptors[i++]);
            CHECK(setenv(key, value, 1) == 0);
        }
        CHECK(setgid(uid) == 0 && setuid(uid) == 0);
        execl(path, path, nullptr);
        _exit(108);
    }
    for (unsigned i = 0; i < count; ++i)
        CHECK(close(descriptors[i]) == 0);
    if (service)
        wait_system_service(child, service);
    return child;
}

static void dump_threads()
{
    DIR *processes = opendir("/proc");
    if (!processes)
        return;
    while (dirent *process = readdir(processes)) {
        if (process->d_name[0] < '1' || process->d_name[0] > '9')
            continue;
        char directory[512];
        snprintf(directory, sizeof(directory), "/proc/%s/task", process->d_name);
        DIR *threads = opendir(directory);
        if (!threads)
            continue;
        while (dirent *thread = readdir(threads)) {
            if (thread->d_name[0] < '1' || thread->d_name[0] > '9')
                continue;
            for (const char *name : {"stat", "wchan", "syscall"}) {
                char path[1024], content[2048];
                snprintf(path, sizeof(path), "%s/%s/%s", directory, thread->d_name, name);
                int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd < 0)
                    continue;
                ssize_t size = read(fd, content, sizeof(content) - 1);
                close(fd);
                if (size <= 0)
                    continue;
                content[size] = 0;
                dprintf(2, "%s: %s\n", path, content);
            }
        }
        closedir(threads);
    }
    closedir(processes);
}

void nce_cgroup_permission_test();

void nce_zygote_test()
{
    bool system_server = access("/data/nce/system-server-probe", F_OK) == 0;
    pid_t health = -1, vold = -1, storaged = -1, artd = -1, netd = -1, lmkd = -1;
    pid_t audio = -1, metrics = -1, audio_hal = -1, allocator = -1, media = -1;
    pid_t gatekeeper = -1, gatekeeper_hal = -1, stats = -1, keymint = -1, keystore = -1;
    AIBinder *artd_reference = nullptr;
    void *setup = dlopen("libprocessgroup_setup.so", RTLD_NOW);
    CHECK(setup);
    auto initialize = reinterpret_cast<bool (*)()>(dlsym(setup, "_Z11CgroupSetupv"));
    CHECK(initialize && initialize());
    CHECK(dlclose(setup) == 0);
    const char *cpu_groups[] = {"", "foreground", "foreground_window", "background", "top-app",
                                "rt", "system", "system-background", "dex2oat"};
    for (const char *group : cpu_groups) {
        char directory[96], path[128];
        snprintf(directory, sizeof(directory), "/dev/cpuctl/%s", group);
        CHECK(mkdir(directory, 0755) == 0 || errno == EEXIST);
        CHECK(chown(directory, 1000, 1000) == 0);
        for (const char *file : {"tasks", "cgroup.procs"}) {
            snprintf(path, sizeof(path), "%s/%s", directory, file);
            CHECK(chown(path, 1000, 1000) == 0 && chmod(path, 0664) == 0);
        }
    }
    if (nce_diagnostics())
        nce_cgroup_permission_test();
    if (system_server) {
        int kernel_log = open("/proc/sys/kernel/printk_devkmsg", O_WRONLY | O_CLOEXEC);
        CHECK(kernel_log >= 0 && write(kernel_log, "on\n", 3) == 3 && close(kernel_log) == 0);
        const struct {
            const char *path;
            mode_t mode;
            uid_t owner;
        } directories[] = {
            {"/data/system", 0775, 1000},
            {"/data/system/users", 0775, 1000},
            {"/data/system/users/0", 0700, 1000},
            {"/data/system_de/0", 0770, 1000},
            {"/data/app", 0771, 1000},
            {"/data/resource-cache", 0771, 1000},
            {"/data/anr", 0775, 1000},
            {"/data/misc/vold", 0700, 0},
            {"/metadata/vold", 0700, 0},
            {"/data/misc/storaged", 0700, 0},
            {"/data/misc/net", 0750, 0},
            {"/data/misc/audioserver", 0700, 1041},
            {"/dev/socket/audioserver", 0775, 1041},
            {"/data/misc/media", 0770, 1013},
            {"/data/misc/gatekeeper", 0700, 1000},
            {"/data/misc/keystore", 0700, 1017},
            {"/data/misc/stats-active-metric", 0770, 1066},
            {"/data/misc/stats-data", 0770, 1066},
            {"/data/misc/stats-data/restricted-data", 0770, 1066},
            {"/data/misc/stats-metadata", 0770, 1066},
            {"/data/misc/stats-service", 0770, 1066},
            {"/data/misc/train-info", 0770, 1066},
        };
        for (const auto &directory : directories) {
            CHECK(mkdir(directory.path, directory.mode) == 0 || errno == EEXIST);
            CHECK(chmod(directory.path, directory.mode) == 0);
            CHECK(chown(directory.path, directory.owner, directory.owner) == 0);
        }
        CHECK(__system_property_set("ro.boot.fake_battery", "1") == 0);
        CHECK(__system_property_set("ro.hardware", "nce") == 0);
        CHECK(__system_property_set("ro.boot.hardware", "nce") == 0);
        CHECK(__system_property_set("ro.crypto.state", "unencrypted") == 0);
        health = nce_start_system_service("/vendor/bin/hw/android.hardware.health-service.example",
                                          "android.hardware.health.IHealth/default", 1000, {});
        // vold requires the helper process contexts supplied by Android init.
        vold = nce_start_system_service("/system/bin/vold", "vold", 0,
                                        {"--blkid_context=u:r:nce_t:s0", "--blkid_untrusted_context=u:r:nce_t:s0",
                                         "--fsck_context=u:r:nce_t:s0", "--fsck_untrusted_context=u:r:nce_t:s0"});
        if (nce_diagnostics())
            nce_framework_check("NceStorage");
        storaged = nce_start_system_service("/system/bin/storaged", "storaged", 0, {});
        artd = nce_start_system_service("/apex/com.android.art/bin/artd", "artd", 0, {});
        // Keep this lazy service alive until the Android session is stopped.
        artd_reference = AServiceManager_checkService("artd");
        CHECK(artd_reference);
        CHECK(mount("bpf", "/sys/fs/bpf", "bpf", 0, nullptr) == 0);
        rlimit locked = {1024 * 1024 * 1024, 1024 * 1024 * 1024};
        CHECK(setrlimit(RLIMIT_MEMLOCK, &locked) == 0);
        pid_t loader = fork();
        CHECK(loader >= 0);
        if (!loader) {
            runtime_environment();
            // Android init's `file /dev/kmsg w` contract survives the loader's exec chain.
            int kmsg = open("/dev/kmsg", O_WRONLY);
            CHECK(kmsg >= 0);
            char descriptor[32];
            snprintf(descriptor, sizeof(descriptor), "%d", kmsg);
            CHECK(setenv("ANDROID_FILE__dev_kmsg", descriptor, 1) == 0);
            execl("/apex/com.android.tethering/bin/netbpfload", "netbpfload", nullptr);
            _exit(108);
        }
        int status;
        CHECK(waitpid(loader, &status, 0) == loader);
        if (status)
            dprintf(2, "network BPF loader status=%d\n", status);
        CHECK(status == 0);
        puts("{\"test\":\"android_network_bpf_loaded\",\"passed\":true}");
        netd = start_socket_service("/system/bin/netd", "netd", {"dnsproxyd", "mdns", "fwmarkd"}, SOCK_STREAM);
        lmkd = start_socket_service("/system/bin/lmkd", nullptr, {"lmkd"}, SOCK_SEQPACKET);
        allocator = nce_start_system_service("/system_ext/bin/hw/android.hidl.allocator@1.0-service", nullptr, 1000, {});
        audio_hal = nce_start_system_service("/vendor/bin/hw/android.hardware.audio.service", nullptr, 1041, {});
        metrics = nce_start_system_service("/system/bin/mediametrics", "media.metrics", 1013, {});
        stats = start_socket_service("/apex/com.android.os.statsd/bin/statsd", "stats", {"statsdw"}, SOCK_DGRAM, 1066, 0222);
        gatekeeper_hal = nce_start_system_service("/vendor/bin/hw/android.hardware.gatekeeper-service.nonsecure",
                                                  "android.hardware.gatekeeper.IGatekeeper/default", 1000, {});
        keymint = nce_start_system_service("/vendor/bin/hw/android.hardware.security.keymint-service",
                                           "android.hardware.security.keymint.IKeyMintDevice/default", 9999, {});
        keystore = nce_start_system_service("/system/bin/keystore2", "android.system.keystore2.IKeystoreService/default", 1017,
                                            {"/data/misc/keystore"});
        gatekeeper = nce_start_system_service("/system/bin/gatekeeperd",
                                              "android.service.gatekeeper.IGateKeeperService", 1000,
                                              {"/data/misc/gatekeeper"});
        if (nce_diagnostics())
            nce_framework_check("NceKeystore");
        // AudioPolicy initialization waits for ActivityManager from system_server.
        audio = nce_start_system_service("/system/bin/audioserver", nullptr, 1041, {});
        media = nce_start_system_service("/system/bin/mediaserver", nullptr, 1013, {});
    }
    CHECK(mkdir("/data/nce/zygote", 0777) == 0 && chmod("/data/nce/zygote", 0777) == 0);
    // Zygote reopens inherited files after fork; append keeps each process's logs intact.
    int log_flags = fcntl(STDERR_FILENO, F_GETFL);
    CHECK(log_flags >= 0 && fcntl(STDERR_FILENO, F_SETFL, log_flags | O_APPEND) == 0);
    const char *zygote_log = "/vendor/zygote_whitelist/nce-stderr";
    CHECK(mount("/stderr", zygote_log, nullptr, MS_BIND, nullptr) == 0);
    int command = control_socket("zygote");
    int usap = control_socket("usap_pool_primary");
    pid_t zygote = fork();
    CHECK(zygote >= 0);
    if (!zygote) {
        runtime_environment();
        // Keep stdin/stdout on init's null device and logs in Zygote's supported vendor path.
        int streams = open("/dev/null", O_RDWR | O_CLOEXEC);
        CHECK(streams > STDERR_FILENO);
        for (int fd = STDIN_FILENO; fd <= STDOUT_FILENO; ++fd)
            CHECK(dup2(streams, fd) == fd);
        CHECK(close(streams) == 0);
        int log = open(zygote_log, O_WRONLY | O_APPEND | O_CLOEXEC);
        CHECK(log >= 0 && dup2(log, STDERR_FILENO) == STDERR_FILENO && close(log) == 0);
        const char *boot = getenv("BOOTCLASSPATH");
        char classpath[16384];
        CHECK(snprintf(classpath, sizeof(classpath), "%s:/system/framework/nce-framework.jar", boot) < static_cast<int>(sizeof(classpath)));
        CHECK(setenv("BOOTCLASSPATH", classpath, 1) == 0);
        char descriptor[32];
        snprintf(descriptor, sizeof(descriptor), "%d", command);
        CHECK(setenv("ANDROID_SOCKET_zygote", descriptor, 1) == 0);
        snprintf(descriptor, sizeof(descriptor), "%d", usap);
        CHECK(setenv("ANDROID_SOCKET_usap_pool_primary", descriptor, 1) == 0);
        if (system_server) {
            execl("/system/bin/app_process64", "app_process64", "-Xzygote", "-Djava.library.path=/system/lib64",
                  "/system/bin", "--zygote", "--start-system-server", "--abi-list=arm64-v8a", nullptr);
            _exit(108);
        }
        execl("/system/bin/app_process64", "app_process64", "-Xzygote", "-Djava.library.path=/system/lib64",
              "/system/bin", "--zygote", "--abi-list=arm64-v8a", "--enable-lazy-preload", nullptr);
        _exit(108);
    }
    CHECK(close(command) == 0 && close(usap) == 0);
    if (system_server) {
        puts("{\"phase\":\"android_system_server_boot\"}");
        const bool interactive = access("/data/nce/desktop-session", F_OK) == 0;
        bool activity_reported = false, services_reported = false, booted = false, stopped = false;
        for (unsigned i = 0;; ++i) {
            if (interactive && access("/data/nce/session-stop", F_OK) == 0) {
                stopped = true;
                break;
            }
            int status;
            CHECK(waitpid(zygote, &status, WNOHANG) == 0);
            for (pid_t service : {audio, audio_hal, metrics, allocator, stats, media, keystore, keymint}) {
                pid_t exited = waitpid(service, &status, WNOHANG);
                if (exited == service)
                    dprintf(2, "Android daemon pid=%d exited status=%d\n", service, status);
                CHECK(exited == 0);
            }
            bool ready = services_reported;
            if (!ready) {
                AIBinder *activity = AServiceManager_checkService("activity");
                AIBinder *package = AServiceManager_checkService("package");
                AIBinder *window = AServiceManager_checkService("window");
                AIBinder *input = AServiceManager_checkService("input");
                AIBinder *lock_settings = AServiceManager_checkService("lock_settings");
                ready = activity && package && window && input && lock_settings;
                if (activity && !activity_reported) {
                    puts("{\"phase\":\"android_activity_service_registered\"}");
                    activity_reported = true;
                }
                if (activity)
                    AIBinder_decStrong(activity);
                if (package)
                    AIBinder_decStrong(package);
                if (window)
                    AIBinder_decStrong(window);
                if (input)
                    AIBinder_decStrong(input);
                if (lock_settings)
                    AIBinder_decStrong(lock_settings);
            }
            if (ready && !services_reported) {
                puts("{\"test\":\"android_system_server_services\",\"passed\":true}");
                services_reported = true;
                // This image starts directly into the desktop without a setup wizard.
                nce_android_command({"settings", "put", "global", "device_provisioned", "1"});
                nce_android_command({"settings", "put", "secure", "user_setup_complete", "1"});
                nce_android_command({"settings", "put", "system", "screen_off_timeout", "2147483647"});
                // Keep the preview's Google Clock APK and data, but expose the
                // AOSP Clock whose local features run without Play services.
                nce_android_command({"package", "disable-user", "--user", "0", "com.google.android.deskclock"}, "disabled-user");
                // Configure the embedded display before SystemUI initializes keyguard.
                nce_android_command({"lock_settings", "set-disabled", "true"}, "Lock screen disabled set to true");
            }
            char completed[PROP_VALUE_MAX] = {};
            __system_property_get("sys.boot_completed", completed);
            if (!booted && ready && !strcmp(completed, "1")) {
                puts("{\"test\":\"android_system_boot_completed\",\"passed\":true}");
                wait_system_service(audio, "media.audio_flinger");
                wait_system_service(audio, "media.audio_policy");
                if (!nce_desktop_start(interactive)) {
                    stopped = true;
                    break;
                }
                booted = true;
                if (interactive)
                    alarm(0);
            }
            if (!booted && i >= 239) {
                dprintf(2, "Android did not complete boot within the 240 second polling window\n");
                CHECK(false);
            }
            if (booted && interactive)
                nce_interaction_profile_poll();
            if (booted && !interactive)
                break;
            usleep(1000000);
        }
        CHECK(booted || stopped);
        if (booted)
            shutdown_framework();
        stop_process_group(zygote);
        AIBinder_decStrong(artd_reference);
        CHECK(__system_property_set("ctl.stop", "idmap2d") == 0);
        for (pid_t service : {health, vold, storaged, artd, netd, lmkd, audio, metrics, audio_hal, allocator, gatekeeper, gatekeeper_hal, stats, media, keystore, keymint})
            stop_process_group(service);
        for (const char *socket : {"dnsproxyd", "mdns", "fwmarkd", "lmkd", "statsdw"}) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/socket/%s", socket);
            CHECK(unlink(path) == 0);
        }
        CHECK(unlink("/dev/socket/zygote") == 0 && unlink("/dev/socket/usap_pool_primary") == 0);
        CHECK(rmdir("/dev/socket/audioserver") == 0);
        puts("{\"test\":\"android_system_process_groups_stopped\",\"passed\":true}");
        if (interactive) {
            printf("{\"session\":\"android_desktop_stopped\",\"boot_completed\":%s}\n", booted ? "true" : "false");
            dprintf(2, "{\"phase\":\"android_desktop_stopped\"}\n");
        }
        return;
    }
    pid_t requester = fork();
    CHECK(requester >= 0);
    if (!requester) {
        CHECK(setgid(1000) == 0 && setuid(1000) == 0);
        int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(client >= 0);
        timeval deadline = {20, 0};
        CHECK(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)) == 0);
        sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        strcpy(address.sun_path, "/dev/socket/zygote");
        CHECK(connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        for (unsigned round = 0; round < 3; ++round) {
            const char *request = round == 1 ? "1\n--get-pid\n" : "1\n--query-abi-list\n";
            CHECK(write(client, request, strlen(request)) == static_cast<ssize_t>(strlen(request)));
            uint32_t length;
            receive_bytes(client, &length, sizeof(length));
            length = ntohl(length);
            char response[64] = {};
            CHECK(length > 0 && length < sizeof(response));
            receive_bytes(client, response, length);
            if (round == 1)
                CHECK(atoi(response) == zygote);
            else
                CHECK(strcmp(response, "arm64-v8a") == 0);
        }
        for (unsigned round = 0; round < 2; ++round) {
            char request[512];
            int bytes = snprintf(request, sizeof(request),
                                 "8\n--runtime-args\n--setuid=1000\n--setgid=1000\n--target-sdk-version=36\n"
                                 "--nice-name=nce-zygote-child\n--seinfo=platform\nNceZygoteChild\n%u\n",
                                 round);
            CHECK(bytes > 0 && bytes < static_cast<int>(sizeof(request)));
            CHECK(write(client, request, bytes) == bytes);
            uint32_t pid;
            unsigned char wrapper;
            receive_bytes(client, &pid, sizeof(pid));
            receive_bytes(client, &wrapper, 1);
            pid = ntohl(pid);
            CHECK(pid > 0 && pid < 32768 && wrapper == 0);
            char path[128];
            snprintf(path, sizeof(path), "/data/nce/zygote/child-%u", round);
            int output = -1;
            for (unsigned attempt = 0; attempt < 500; ++attempt) {
                output = open(path, O_RDONLY | O_CLOEXEC);
                if (output >= 0)
                    break;
                CHECK(errno == ENOENT);
                usleep(10000);
            }
            CHECK(output >= 0);
            char result[128] = {};
            CHECK(read(output, result, sizeof(result) - 1) > 0 && close(output) == 0);
            char expected[128];
            snprintf(expected, sizeof(expected), "%u:%d:1000:4096\n", pid, zygote);
            CHECK(strcmp(result, expected) == 0);
        }
        CHECK(close(client) == 0);
        _exit(0);
    }
    int requested;
    CHECK(waitpid(requester, &requested, 0) == requester);
    if (requested)
        dump_threads();
    CHECK(requested == 0);
    CHECK(kill(zygote, SIGTERM) == 0);
    int status;
    CHECK(waitpid(zygote, &status, 0) == zygote && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(unlink("/dev/socket/zygote") == 0 && unlink("/dev/socket/usap_pool_primary") == 0);
    puts("{\"test\":\"android_zygote_command_server\",\"queries\":3,\"passed\":true}");
    puts("{\"test\":\"android_zygote_java_children\",\"children\":2,\"passed\":true}");
}
