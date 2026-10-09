/* SPDX-License-Identifier: Apache-2.0 */
#include <aidl/android/os/IInstalld.h>
#include <android/binder_manager.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

using namespace aidl::android::os;
void nce_android_storage_prepare();
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "installd-test:%d uid=%d errno=%d: %s\n", __LINE__, getuid(), errno, #c); \
            _exit(114); \
        } \
    } while (0)
static constexpr int kStorage = IInstalld::FLAG_STORAGE_CE | IInstalld::FLAG_STORAGE_DE;
static const char *kPackages[] = {"org.nce.first", "org.nce.second"};

static void check_status(const ndk::ScopedAStatus &status)
{
    if (!status.isOk())
        dprintf(2, "installd: %s\n", status.getDescription().c_str());
    CHECK(status.isOk());
}

void nce_installd_client(const char *stage)
{
    auto service = IInstalld::fromBinder(ndk::SpAIBinder(AServiceManager_checkService("installd")));
    CHECK(service && service->isRemote());
    if (!strcmp(stage, "app")) {
        CHECK(getuid() == 10001);
        auto denied = service->createUserData(std::nullopt, 0, 0, kStorage);
        CHECK(denied.getExceptionCode() == EX_SECURITY);
        for (const char *base : {"/data/user/0", "/data/user_de/0"}) {
            for (const char *name : {"payload", "cache/payload", "code_cache/payload"}) {
                std::string path = std::string(base) + "/" + kPackages[0] + "/" + name;
                int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
                CHECK(fd >= 0 && write(fd, "nce-data", 8) == 8 && close(fd) == 0);
            }
            std::string other = std::string(base) + "/" + kPackages[1] + "/payload";
            CHECK(open(other.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600) == -1 && errno == EACCES);
        }
        return;
    }
    CHECK(getuid() == 1000);
    if (!strcmp(stage, "create")) {
        check_status(service->createUserData(std::nullopt, 0, 0, kStorage));
        std::vector<CreateAppDataArgs> args(2);
        for (unsigned i = 0; i < args.size(); ++i) {
            args[i].packageName = kPackages[i];
            args[i].userId = 0;
            args[i].flags = kStorage;
            args[i].appId = 10001 + i;
            args[i].previousAppId = -1;
            args[i].seInfo = "default";
            args[i].targetSdkVersion = 36;
        }
        std::vector<CreateAppDataResult> results;
        check_status(service->createAppDataBatched(args, &results));
        CHECK(results.size() == args.size());
        for (const auto &result : results) {
            if (result.exceptionCode)
                dprintf(2, "createAppData: %s\n", result.exceptionMessage.c_str());
            CHECK(result.exceptionCode == 0 && result.ceDataInode > 0 && result.deDataInode > 0);
        }
        CreateAppDataResult invalid;
        args[0].packageName = "../nce";
        check_status(service->createAppData(args[0], &invalid));
        CHECK(invalid.exceptionCode == EX_ILLEGAL_ARGUMENT);
        CHECK(service->createUserData(std::string("../escape"), 0, 0, kStorage).getExceptionCode() == EX_ILLEGAL_ARGUMENT);
    } else if (!strcmp(stage, "cache")) {
        check_status(service->clearAppData(std::nullopt, kPackages[0], 0,
                                           kStorage | IInstalld::FLAG_CLEAR_CACHE_ONLY, 0));
    } else if (!strcmp(stage, "clear")) {
        check_status(service->clearAppData(std::nullopt, kPackages[0], 0, kStorage, 0));
    } else {
        CHECK(!strcmp(stage, "destroy"));
        for (const char *package : kPackages)
            check_status(service->destroyAppData(std::nullopt, package, 0, kStorage, 0));
    }
}

static void client(const char *executable, const char *stage, uid_t uid = 1000)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setgroups(0, nullptr) == 0 && setgid(uid) == 0 && setuid(uid) == 0);
        /* exec gives the client its own Binder driver state after fork. */
        execl(executable, executable, "installd-test", stage, nullptr);
        _exit(115);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
}

pid_t nce_installd_start(const char *executable)
{
    nce_android_storage_prepare();
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(setenv("ANDROID_DATA", "/data", 1) == 0 && setenv("ANDROID_ROOT", "/system", 1) == 0);
        CHECK(setenv("ASEC_MOUNTPOINT", "/mnt/asec", 1) == 0);
        CHECK(setenv("LD_PRELOAD", "/system/lib64/libnce-framework-log.so", 1) == 0);
        execl("/system/bin/installd", "installd", nullptr);
        _exit(116);
    }
    ndk::SpAIBinder service;
    for (unsigned attempt = 0; attempt < 200 && !service.get(); ++attempt) {
        int status;
        CHECK(waitpid(child, &status, WNOHANG) == 0);
        service = ndk::SpAIBinder(AServiceManager_checkService("installd"));
        if (!service.get())
            usleep(10000);
    }
    CHECK(service.get() && AIBinder_ping(service.get()) == STATUS_OK);
    client(executable, "create");
    for (const char *base : {"/data/user/0", "/data/user_de/0"}) {
        for (unsigned i = 0; i < 2; ++i) {
            std::string path = std::string(base) + "/" + kPackages[i];
            struct stat info;
            CHECK(stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode));
            CHECK(info.st_uid == 10001 + i && info.st_gid == 10001 + i && (info.st_mode & 07777) == 0700);
            char label[128] = {};
            CHECK(getxattr(path.c_str(), "security.selinux", label, sizeof(label)) > 0);
            CHECK(!strcmp(label, "u:object_r:nce_t:s0"));
            path += "/cache";
            CHECK(stat(path.c_str(), &info) == 0 && info.st_uid == 10001 + i && info.st_gid == 20001 + i);
            CHECK((info.st_mode & 07777) == 02771);
        }
    }
    client(executable, "app", 10001);
    client(executable, "cache");
    for (const char *base : {"/data/user/0", "/data/user_de/0"}) {
        std::string path = std::string(base) + "/" + kPackages[0];
        CHECK(access((path + "/payload").c_str(), F_OK) == 0);
        CHECK(access((path + "/code_cache/payload").c_str(), F_OK) == 0);
        CHECK(access((path + "/cache/payload").c_str(), F_OK) == -1 && errno == ENOENT);
    }
    client(executable, "clear");
    for (const char *base : {"/data/user/0", "/data/user_de/0"}) {
        std::string path = std::string(base) + "/" + kPackages[0];
        CHECK(access(path.c_str(), F_OK) == 0);
        CHECK(access((path + "/payload").c_str(), F_OK) == -1 && errno == ENOENT);
        CHECK(access((path + "/code_cache/payload").c_str(), F_OK) == -1 && errno == ENOENT);
    }
    client(executable, "destroy");
    for (const char *base : {"/data/user/0", "/data/user_de/0"}) {
        for (const char *package : kPackages) {
            std::string path = std::string(base) + "/" + package;
            CHECK(access(path.c_str(), F_OK) == -1 && errno == ENOENT);
        }
    }
    puts("{\"test\":\"android_installd_app_data\",\"passed\":true,\"packages\":2,\"storage_types\":2,\"caller_uid\":1000}");
    return child;
}
