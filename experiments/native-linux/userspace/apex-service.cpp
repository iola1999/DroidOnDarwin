/* SPDX-License-Identifier: Apache-2.0 */
#include <aidl/android/apex/BnApexService.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "apex-modules.h"

using namespace aidl::android::apex;
using ndk::ScopedAStatus;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "apex-service:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(110); \
        } \
    } while (0)

/* The system image contains a fixed module set. Generated AIDL defaults return
 * UNKNOWN_TRANSACTION for installation and rollback operations. */
class ApexRegistry final : public IApexServiceDefault
{
    std::vector<ApexInfo> modules_;

   public:
    ApexRegistry()
    {
        for (const auto &seed : apex_modules) {
            ApexInfo info;
            info.moduleName = seed.name;
            info.modulePath = std::string("/system/apex/") + seed.name + ".apex";
            info.preinstalledModulePath = info.modulePath;
            info.versionCode = seed.version;
            info.versionName = seed.version_name;
            info.isFactory = true;
            info.isActive = true;
            info.activeApexChanged = false;
            struct stat status;
            CHECK(stat(info.modulePath.c_str(), &status) == 0 && status.st_size == seed.archive_size);
            std::string manifest = std::string("/apex/") + seed.name + "/apex_manifest.pb";
            CHECK(stat(manifest.c_str(), &status) == 0 && status.st_size > 0);
            modules_.push_back(std::move(info));
        }
    }
    ScopedAStatus getActivePackages(std::vector<ApexInfo> *result) override
    {
        *result = modules_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getAllPackages(std::vector<ApexInfo> *result) override
    {
        return getActivePackages(result);
    }
    ScopedAStatus getSessions(std::vector<ApexSessionInfo> *result) override
    {
        result->clear();
        return ScopedAStatus::ok();
    }
    ScopedAStatus getStagedSessionInfo(int32_t id, ApexSessionInfo *result) override
    {
        result->sessionId = id;
        result->isUnknown = true;
        return ScopedAStatus::ok();
    }
};

void nce_apex_service(int ready)
{
    CHECK(setgid(1000) == 0 && setuid(1000) == 0);
    auto registry = ndk::SharedRefBase::make<ApexRegistry>();
    auto service = ndk::SharedRefBase::make<IApexServiceDelegator>(registry);
    CHECK(AServiceManager_addService(service->asBinder().get(), "apexservice") == STATUS_OK);
    CHECK(write(ready, "r", 1) == 1 && close(ready) == 0);
    ABinderProcess_joinThreadPool();
}

void nce_apex_test()
{
    ndk::SpAIBinder binder(AServiceManager_checkService("apexservice"));
    auto service = IApexService::fromBinder(binder);
    CHECK(service && service->isRemote());
    std::vector<ApexInfo> active, installed;
    CHECK(service->getActivePackages(&active).isOk() && service->getAllPackages(&installed).isOk());
    CHECK(active == installed && active.size() == sizeof(apex_modules) / sizeof(apex_modules[0]));
    for (size_t i = 0; i < active.size(); ++i) {
        CHECK(active[i].moduleName == apex_modules[i].name && active[i].versionCode == apex_modules[i].version);
        CHECK(active[i].versionName == apex_modules[i].version_name && active[i].isActive && active[i].isFactory);
        int fd = open(active[i].modulePath.c_str(), O_RDONLY | O_CLOEXEC);
        char magic[4];
        CHECK(fd >= 0 && read(fd, magic, sizeof(magic)) == 4 && magic[0] == 'P' && magic[1] == 'K');
        CHECK(close(fd) == 0);
    }
    std::vector<ApexSessionInfo> sessions;
    CHECK(service->getSessions(&sessions).isOk() && sessions.empty());
    ApexSessionInfo unknown;
    CHECK(service->getStagedSessionInfo(123, &unknown).isOk() && unknown.sessionId == 123 && unknown.isUnknown);
    ApexInfo ignored;
    CHECK(!service->installAndActivatePackage(active[0].modulePath, false, &ignored).isOk());
    printf("{\"test\":\"android_apex_registry\",\"passed\":true,\"modules\":%zu,\"mutation_rejected\":true}\n", active.size());
}
