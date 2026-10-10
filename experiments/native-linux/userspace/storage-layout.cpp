/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "apex-modules.h"

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "storage-layout:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(118); \
        } \
    } while (0)

static bool exists(const char *path)
{
    struct stat info;
    if (lstat(path, &info) == 0) {
        CHECK(S_ISDIR(info.st_mode));
        return true;
    }
    CHECK(errno == ENOENT);
    return false;
}

void nce_android_storage_prepare()
{
    // Older prototypes stored user 0 directly here; rename preserves its inode and contents.
    if (!exists("/data/data")) {
        if (exists("/data/user/0"))
            CHECK(rename("/data/user/0", "/data/data") == 0);
        else
            CHECK(mkdir("/data/data", 0771) == 0);
    }
    const struct {
        const char *path;
        mode_t mode;
        uid_t uid;
        gid_t gid;
    } directories[] = {
        {"/mnt", 0755, 0, 0},
        {"/mnt/secure", 0700, 0, 0},
        {"/mnt/secure/asec", 0700, 0, 0},
        {"/mnt/asec", 0755, 0, 1000},
        {"/mnt/obb", 0755, 0, 1000},
        {"/mnt/media_rw", 0750, 0, 1077},
        {"/mnt/user", 0755, 0, 0},
        {"/mnt/user/0", 0755, 0, 0},
        {"/mnt/user/0/self", 0755, 0, 0},
        {"/mnt/user/0/emulated", 0755, 0, 0},
        {"/mnt/user/0/emulated/0", 0755, 0, 0},
        {"/mnt/pass_through", 0700, 0, 0},
        {"/mnt/pass_through/0", 0710, 0, 1023},
        {"/mnt/pass_through/0/self", 0710, 0, 1023},
        {"/mnt/pass_through/0/emulated", 0710, 0, 1023},
        {"/mnt/pass_through/0/emulated/0", 0710, 0, 1023},
        {"/mnt/installer", 0755, 0, 0},
        {"/mnt/androidwritable", 0755, 0, 0},
        {"/mnt/expand", 0771, 1000, 1000},
        {"/mnt/appfuse", 0711, 0, 0},
        {"/mnt/runtime", 0700, 0, 0},
        {"/mnt/runtime/default", 0755, 0, 0},
        {"/mnt/runtime/default/self", 0755, 0, 0},
        {"/mnt/runtime/read", 0755, 0, 0},
        {"/mnt/runtime/read/self", 0755, 0, 0},
        {"/mnt/runtime/write", 0755, 0, 0},
        {"/mnt/runtime/write/self", 0755, 0, 0},
        {"/mnt/runtime/full", 0755, 0, 0},
        {"/mnt/runtime/full/self", 0755, 0, 0},
        {"/storage", 0755, 0, 0},
        {"/data/misc", 01771, 1000, 9998},
        {"/data/misc/user", 0755, 0, 0},
        {"/data/misc/installd", 0700, 0, 0},
        {"/data/misc/apexdata", 0711, 0, 0},
        {"/data/user", 0711, 1000, 1000},
        {"/data/user/0", 0771, 1000, 1000},
        {"/data/user_de", 0711, 1000, 1000},
        {"/data/user_de/0", 0771, 1000, 1000},
        {"/data/misc/profiles", 0771, 1000, 1000},
        {"/data/misc/profiles/cur", 0771, 1000, 1000},
        {"/data/misc/profiles/cur/0", 0771, 1000, 1000},
        {"/data/misc/profiles/ref", 0771, 1000, 1000},
    };
    for (const auto &directory : directories) {
        CHECK(mkdir(directory.path, directory.mode) == 0 || errno == EEXIST);
        CHECK(chmod(directory.path, directory.mode) == 0 && chown(directory.path, directory.uid, directory.gid) == 0);
    }
    CHECK(symlink("/storage/self/primary", "/mnt/sdcard") == 0);
    CHECK(symlink("/mnt/user/0/primary", "/mnt/runtime/default/self/primary") == 0);
    // These slave mounts receive vold mounts without propagating app-specific views back.
    CHECK(mount(nullptr, "/", nullptr, MS_SHARED | MS_REC, nullptr) == 0);
    for (const char *path : {"/mnt/installer", "/mnt/androidwritable"}) {
        CHECK(mount("/mnt/user", path, nullptr, MS_BIND | MS_REC, nullptr) == 0);
        CHECK(mount(nullptr, path, nullptr, MS_SLAVE, nullptr) == 0);
        CHECK(mount(nullptr, path, nullptr, MS_SHARED, nullptr) == 0);
    }
    CHECK(mount("/mnt/user/0", "/storage", nullptr, MS_BIND | MS_REC, nullptr) == 0);
    CHECK(mount(nullptr, "/storage", nullptr, MS_SLAVE | MS_REC, nullptr) == 0);
    // vold uses this inventory to prepare each module's CE/DE directories.
    for (const auto &module : apex_modules) {
        char path[256];
        CHECK(snprintf(path, sizeof(path), "/data/misc/apexdata/%s", module.name) < (int)sizeof(path));
        CHECK(mkdir(path, 0771) == 0 || errno == EEXIST);
        CHECK(chmod(path, 0771) == 0 && chown(path, 0, 1000) == 0);
    }

    CHECK(chmod("/data/data", 0771) == 0 && chown("/data/data", 1000, 1000) == 0);
    DIR *target = opendir("/data/user/0");
    CHECK(target);
    while (dirent *entry = readdir(target))
        CHECK(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
    CHECK(closedir(target) == 0);
    CHECK(mount(nullptr, "/data", nullptr, MS_SHARED | MS_REC, nullptr) == 0);
    CHECK(mount("/data/data", "/data/user/0", nullptr, MS_BIND, nullptr) == 0);
    const struct {
        const char *path;
        uid_t owner;
        gid_t group;
        mode_t mode;
    } extra[] = {
        {"/data/media", 1023, 1023, 0550},
        {"/data/misc_ce", 1000, 9998, 0551},
        {"/data/misc_de", 1000, 9998, 0551},
        {"/data/system_ce", 1000, 1000, 0550},
        {"/data/system_de", 1000, 1000, 0550},
        {"/data/vendor_ce", 0, 0, 0551},
        {"/data/vendor_de", 0, 0, 0551},
        {"/data/storage_area", 0, 0, 0551},
        {"/data_mirror", 0, 1000, 0700},
    };
    for (const auto &dir : extra) {
        CHECK(mkdir(dir.path, dir.mode) == 0 || errno == EEXIST);
        CHECK(chmod(dir.path, dir.mode) == 0 && chown(dir.path, dir.owner, dir.group) == 0);
    }
    CHECK(mount("tmpfs", "/data_mirror", "tmpfs", MS_NODEV | MS_NOEXEC | MS_NOSUID, "mode=0700,uid=0,gid=1000") == 0);
    const char *parents[] = {"data_ce", "data_de", "misc_ce", "misc_de"};
    for (const char *parent : parents) {
        char path[128];
        snprintf(path, sizeof(path), "/data_mirror/%s", parent);
        CHECK(mkdir(path, 0700) == 0);
    }
    const char *binds[][2] = {
        {"/data/user", "/data_mirror/data_ce/null"},
        {"/data/user_de", "/data_mirror/data_de/null"},
        {"/data/misc_ce", "/data_mirror/misc_ce/null"},
        {"/data/misc_de", "/data_mirror/misc_de/null"},
        {"/data/storage_area", "/data_mirror/storage_area"},
        {"/data/misc/profiles/cur", "/data_mirror/cur_profiles"},
        {"/data/misc/profiles/ref", "/data_mirror/ref_profiles"},
    };
    for (const auto &bind : binds) {
        CHECK(mkdir(bind[1], 0700) == 0);
        CHECK(mount(bind[0], bind[1], nullptr, MS_BIND | MS_REC, nullptr) == 0);
        struct stat source, mirror;
        CHECK(stat(bind[0], &source) == 0 && stat(bind[1], &mirror) == 0);
        CHECK(source.st_dev == mirror.st_dev && source.st_ino == mirror.st_ino);
    }
    struct stat legacy, user, mirror;
    CHECK(stat("/data/data", &legacy) == 0 && stat("/data/user/0", &user) == 0 && stat("/data_mirror/data_ce/null/0", &mirror) == 0);
    CHECK(legacy.st_dev == user.st_dev && legacy.st_ino == user.st_ino && user.st_dev == mirror.st_dev && user.st_ino == mirror.st_ino);
    puts("{\"test\":\"android_app_data_mount_layout\",\"passed\":true}");
}
