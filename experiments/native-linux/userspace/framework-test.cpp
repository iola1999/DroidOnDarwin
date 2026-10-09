/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
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
        {"CLASSPATH", "/data/nce/NceFramework.dex"},
        {"LD_PRELOAD", "/system/lib64/libnce-framework-log.so"},
        {"BOOTCLASSPATH", "/apex/com.android.art/javalib/core-oj.jar:"
            "/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:"
            "/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:"
            "/apex/com.android.i18n/javalib/core-icu4j.jar:/apex/com.android.conscrypt/javalib/conscrypt.jar:"
            "/system/framework/framework.jar:/system/framework/framework-graphics.jar:"
            "/system/framework/framework-location.jar:/system/framework/ext.jar:"
            "/apex/com.android.tethering/javalib/framework-connectivity.jar:"
            "/apex/com.android.tethering/javalib/framework-connectivity-b.jar:"
            "/apex/com.android.tethering/javalib/framework-connectivity-t.jar:"
            "/apex/com.android.tethering/javalib/framework-tethering.jar:"
            "/apex/com.android.configinfrastructure/javalib/framework-configinfrastructure.jar:"
            "/apex/com.android.os.statsd/javalib/framework-statsd.jar"},
    };
    for (const auto &entry : environment) CHECK(setenv(entry[0], entry[1], 1) == 0);
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
        {"dalvik.vm.extra-opts", "-Xnoimage-dex2oat -Xcompiler-option --inline-max-code-units=0"},
    };
    for (const auto &entry : properties) CHECK(__system_property_set(entry[0], entry[1]) == 0);
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
    int command = control_socket("zygote");
    int usap = control_socket("usap_pool_primary");
    pid_t zygote = fork();
    CHECK(zygote >= 0);
    if (!zygote) {
        runtime_environment();
        char descriptor[32];
        snprintf(descriptor, sizeof(descriptor), "%d", command);
        CHECK(setenv("ANDROID_SOCKET_zygote", descriptor, 1) == 0);
        snprintf(descriptor, sizeof(descriptor), "%d", usap);
        CHECK(setenv("ANDROID_SOCKET_usap_pool_primary", descriptor, 1) == 0);
        execl("/system/bin/app_process64", "app_process64", "-Xzygote", "-Djava.library.path=/system/lib64",
              "/system/bin", "--zygote", "--abi-list=arm64-v8a", "--enable-lazy-preload", nullptr);
        _exit(108);
    }
    CHECK(close(command) == 0 && close(usap) == 0);
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
}
