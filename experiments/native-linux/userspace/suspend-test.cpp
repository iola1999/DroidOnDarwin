/* SPDX-License-Identifier: Apache-2.0 */
#include "diagnostics.h"
#include <aidl/android/system/suspend/ISystemSuspend.h>
#include <android/binder_manager.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace aidl::android::system::suspend;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "suspend-test:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(112); \
        } \
    } while (0)

pid_t nce_suspend_start()
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setenv("LD_PRELOAD", "/system/lib64/libnce-framework-log.so", 1) == 0);
        CHECK(setgid(1000) == 0 && setuid(1000) == 0);
        execl("/system/bin/hw/android.system.suspend-service", "android.system.suspend-service", nullptr);
        _exit(113);
    }
    std::shared_ptr<ISystemSuspend> service;
    for (unsigned attempt = 0; attempt < 200 && !service; ++attempt) {
        int status;
        CHECK(waitpid(child, &status, WNOHANG) == 0);
        ndk::SpAIBinder binder(AServiceManager_checkService("android.system.suspend.ISystemSuspend/default"));
        service = ISystemSuspend::fromBinder(binder);
        if (!service)
            usleep(10000);
    }
    CHECK(service && service->isRemote());
    if (!nce_diagnostics())
        return child;
    std::shared_ptr<IWakeLock> first, second;
    CHECK(service->acquireWakeLock(WakeLockType::PARTIAL, "nce-first", &first).isOk() && first);
    CHECK(service->acquireWakeLock(WakeLockType::FULL, "nce-second", &second).isOk() && second);
    CHECK(first->release().isOk() && first->release().isOk());
    second.reset();
    first.reset();
    CHECK(service->acquireWakeLock(WakeLockType::PARTIAL, "nce-reacquire", &first).isOk() && first);
    CHECK(first->release().isOk());
    CHECK(AIBinder_ping(service->asBinder().get()) == STATUS_OK);
    puts("{\"test\":\"android_system_suspend_wakelocks\",\"passed\":true,\"acquisitions\":3,\"repeat_release\":true}");
    return child;
}
