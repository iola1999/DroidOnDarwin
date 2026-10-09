/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

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
        {"/data/misc", 01771, 1000, 9998},
        {"/data/misc/user", 0755, 0, 0},
        {"/data/misc/installd", 0700, 0, 0},
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
        {"/data/misc_ce", 1000, 9998, 0551},
        {"/data/misc_de", 1000, 9998, 0551},
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
