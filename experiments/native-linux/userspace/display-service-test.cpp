/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <android/binder_manager.h>
#include <android/binder_ibinder.h>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "display-service:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(115); \
        } \
    } while (0)

static pid_t allocator, composer, surfaceflinger;

static void wait_for_service(pid_t child, const char *path, const char *service)
{
    for (unsigned i = 0; i < 300; ++i) {
        int status;
        if (waitpid(child, &status, WNOHANG) != 0) {
            dprintf(2, "%s exited: status=%d\n", path, status);
            CHECK(false);
        }
        AIBinder *binder = AServiceManager_checkService(service);
        if (binder) {
            AIBinder_decStrong(binder);
            printf("{\"phase\":\"android_display_service_registered\",\"service\":\"%s\"}\n", service);
            return;
        }
        usleep(100000);
    }
    dprintf(2, "%s did not register %s within 30 seconds\n", path, service);
    CHECK(false);
}

static pid_t start(const char *path, const char *service)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setenv("LD_PRELOAD", "/system/lib64/libnce-qemupipe.so:/system/lib64/libnce-framework-log.so", 1) == 0);
        CHECK(setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/vendor/lib64/egl:/vendor/lib64/hw", 1) == 0);
        /* Exercise Binder and graphics permissions with Android's system UID. */
        CHECK(setgid(1000) == 0 && setuid(1000) == 0);
        execl(path, path, nullptr);
        _exit(116);
    }
    wait_for_service(child, path, service);
    return child;
}

void nce_display_start()
{
    CHECK(__system_property_set("ro.hardware.egl", "emulation") == 0);
    CHECK(__system_property_set("debug.renderengine.backend", "skiaglthreaded") == 0);
    CHECK(__system_property_set("debug.sf.nobootanimation", "1") == 0);
    CHECK(__system_property_set("service.sf.prime_shader_cache", "false") == 0);
    CHECK(__system_property_set("ro.sf.lcd_density", "160") == 0);
    CHECK(__system_property_set("ro.boot.qemu.vsync", "60") == 0);
    allocator = start("/vendor/bin/hw/android.hardware.graphics.allocator-service.nce",
                      "android.hardware.graphics.allocator.IAllocator/default");
    composer = start("/vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu",
                     "android.hardware.graphics.composer3.IComposer/default");
    surfaceflinger = start("/system/bin/surfaceflinger", "SurfaceFlinger");
    /* SurfaceFlinger publishes its two Binder services in separate calls. */
    wait_for_service(surfaceflinger, "/system/bin/surfaceflinger", "SurfaceFlingerAIDL");
    puts("{\"test\":\"android_surfaceflinger_registered\",\"passed\":true}");
}

void nce_display_stop()
{
    for (pid_t child : {surfaceflinger, composer, allocator}) {
        int status;
        CHECK(kill(child, SIGTERM) == 0 && waitpid(child, &status, 0) == child);
    }
}
