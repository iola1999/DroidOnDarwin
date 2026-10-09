/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <android/binder_manager.h>
#include <android/binder_ibinder.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <initializer_list>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "framework-test:%d errno=%d: %s\n", __LINE__, errno, #c); _exit(109); } } while (0)

static void runtime_environment()
{
    const char *environment[][2] = {
        {"ANDROID_ROOT", "/system"}, {"ANDROID_DATA", "/data"},
        {"ANDROID_ART_ROOT", "/apex/com.android.art"},
        {"ANDROID_I18N_ROOT", "/apex/com.android.i18n"},
        {"ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata"},
        {"ICU_DATA", "/apex/com.android.i18n/etc/icu"},
        {"CLASSPATH", "/system/framework/nce-framework.jar"},
        {"LD_PRELOAD", "/system/lib64/libnce-framework-log.so"},
    };
    for (const auto &entry : environment) CHECK(setenv(entry[0], entry[1], 1) == 0);
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

void nce_framework_test()
{
    const char *properties[][2] = {
        {"ro.product.cpu.abilist64", "arm64-v8a"}, {"ro.product.cpu.abilist", "arm64-v8a"},
        {"ro.product.cpu.abi", "arm64-v8a"}, {"ro.build.version.sdk", "36"},
        {"ro.build.version.sdk_full", "36.0"}, {"ro.build.version.release", "16"},
        {"ro.build.version.release_or_codename", "16"}, {"ro.build.version.codename", "REL"},
        {"ro.build.version.all_codenames", "REL"}, {"ro.product.model", "NCE prototype"},
        {"ro.treble.enabled", "true"}, {"sys.use_memfd", "true"},
        {"dalvik.vm.heapstartsize", "16m"}, {"dalvik.vm.heapsize", "128m"},
        {"dalvik.vm.jdwp-provider", "none"},
        {"dalvik.vm.usejit", "true"}, {"dalvik.vm.jitmaxsize", "32m"},
        {"dalvik.vm.jitthreshold", "65535"}, {"dalvik.vm.gctype", "MS"},
        {"dalvik.vm.boot-image", "/data/no-boot-image.art"},
        {"dalvik.vm.extra-opts", "-Xnoimage-dex2oat"},
    };
    for (const auto &entry : properties) CHECK(__system_property_set(entry[0], entry[1]) == 0);
    derive_classpath();
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        runtime_environment();
        execl("/system/bin/app_process64", "app_process64", "-Djava.library.path=/system/lib64",
              "/system/bin", "NceFramework", nullptr);
        _exit(108);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    if (status) dprintf(2, "framework-test child=%d status=%d\n", child, status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int control_socket(const char *name)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    CHECK(snprintf(address.sun_path, sizeof(address.sun_path), "/dev/socket/%s", name) > 0);
    CHECK(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    CHECK(chown(address.sun_path, 0, 1000) == 0 && chmod(address.sun_path, 0660) == 0);
    CHECK(listen(fd, 8) == 0);
    return fd;
}

static void receive_bytes(int fd, void *buffer, size_t size)
{
    while (size) {
        ssize_t count = read(fd, buffer, size);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0);
        buffer = static_cast<char *>(buffer) + count;
        size -= count;
    }
}

void nce_zygote_test()
{
    bool system_server = access("/data/nce/system-server-probe", F_OK) == 0;
    CHECK(symlink("/system/etc", "/etc") == 0);
    if (system_server) {
        const struct { const char *path; mode_t mode; uid_t owner; } directories[] = {
            {"/mnt", 0755, 0}, {"/mnt/user", 0755, 0}, {"/mnt/user/0", 0755, 0},
            {"/storage", 0755, 0}, {"/system_ext", 0755, 0},
            {"/product", 0755, 0}, {"/vendor", 0755, 0}, {"/data/system", 0775, 1000},
            {"/data/system/users", 0775, 1000}, {"/data/system/users/0", 0700, 1000},
            {"/data/system_de", 0770, 1000}, {"/data/system_de/0", 0770, 1000},
            {"/data/misc", 01771, 1000}, {"/data/resource-cache", 0771, 1000},
            {"/data/anr", 0775, 1000},
        };
        for (const auto &directory : directories) {
            CHECK(mkdir(directory.path, directory.mode) == 0 || errno == EEXIST);
            CHECK(chmod(directory.path, directory.mode) == 0);
            CHECK(chown(directory.path, directory.owner, directory.owner) == 0);
        }
    }
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
    CHECK(mkdir("/data/nce/zygote", 0777) == 0 && chmod("/data/nce/zygote", 0777) == 0);
    int command = control_socket("zygote");
    int usap = control_socket("usap_pool_primary");
    pid_t zygote = fork();
    CHECK(zygote >= 0);
    if (!zygote) {
        runtime_environment();
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
                  "/system/bin", "--zygote", "--start-system-server", "--abi-list=arm64-v8a", "--enable-lazy-preload", nullptr);
            _exit(108);
        }
        execl("/system/bin/app_process64", "app_process64", "-Xzygote", "-Djava.library.path=/system/lib64",
              "/system/bin", "--zygote", "--abi-list=arm64-v8a", "--enable-lazy-preload", nullptr);
        _exit(108);
    }
    CHECK(close(command) == 0 && close(usap) == 0);
    if (system_server) {
        puts("{\"phase\":\"android_system_server_boot\"}");
        for (unsigned i = 0; i < 400; ++i) {
            int status;
            CHECK(waitpid(zygote, &status, WNOHANG) == 0);
            AIBinder *activity = AServiceManager_checkService("activity");
            AIBinder *package = AServiceManager_checkService("package");
            bool ready = activity && package;
            if (activity) AIBinder_decStrong(activity);
            if (package) AIBinder_decStrong(package);
            if (ready) {
                puts("{\"test\":\"android_system_server_services\",\"passed\":true}");
                CHECK(kill(zygote, SIGTERM) == 0 && waitpid(zygote, &status, 0) == zygote);
                return;
            }
            usleep(100000);
        }
        dprintf(2, "system_server did not publish activity and package services within 40 seconds\n");
        CHECK(false);
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
            if (round == 1) CHECK(atoi(response) == zygote);
            else CHECK(strcmp(response, "arm64-v8a") == 0);
        }
        for (unsigned round = 0; round < 2; ++round) {
            char request[512];
            int bytes = snprintf(request, sizeof(request),
                "8\n--runtime-args\n--setuid=1000\n--setgid=1000\n--target-sdk-version=36\n"
                "--nice-name=nce-zygote-child\n--seinfo=platform\nNceZygoteChild\n%u\n", round);
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
                if (output >= 0) break;
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
    CHECK(waitpid(requester, &requested, 0) == requester && requested == 0);
    CHECK(kill(zygote, SIGTERM) == 0);
    int status;
    CHECK(waitpid(zygote, &status, 0) == zygote && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(unlink("/dev/socket/zygote") == 0 && unlink("/dev/socket/usap_pool_primary") == 0);
    puts("{\"test\":\"android_zygote_command_server\",\"queries\":3,\"passed\":true}");
    puts("{\"test\":\"android_zygote_java_children\",\"children\":2,\"passed\":true}");
}
