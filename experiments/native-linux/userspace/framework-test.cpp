/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "framework-test:%d errno=%d: %s\n", __LINE__, errno, #c); _exit(109); } } while (0)

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
        {"dalvik.vm.usejit", "true"}, {"dalvik.vm.jitmaxsize", "32m"},
        {"dalvik.vm.jitthreshold", "65535"}, {"dalvik.vm.gctype", "MS"},
        {"dalvik.vm.boot-image", "/data/no-boot-image.art"},
        {"dalvik.vm.extra-opts", "-Xnoimage-dex2oat -Xcompiler-option --inline-max-code-units=0"},
    };
    for (const auto &entry : properties) CHECK(__system_property_set(entry[0], entry[1]) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
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
                "/apex/com.android.configinfrastructure/javalib/framework-configinfrastructure.jar"},
        };
        for (const auto &entry : environment) CHECK(setenv(entry[0], entry[1], 1) == 0);
        execl("/system/bin/app_process64", "app_process64", "-Djava.library.path=/system/lib64",
              "/system/bin", "NceFramework", nullptr);
        _exit(108);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    if (status) dprintf(2, "framework-test child=%d status=%d\n", child, status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
