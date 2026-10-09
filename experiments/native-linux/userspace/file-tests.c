/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "file-tests:%d errno=%d: %s\n", __LINE__, errno, #c); _exit(111); } } while (0)
void file_tests(const char *libc)
{
    CHECK(mkdir("/open-flags", 0700) == 0);
    int file = open("/open-flags/data", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    CHECK(file >= 0 && fcntl(file, F_GETFD) == FD_CLOEXEC && close(file) == 0);
    CHECK(symlink("data", "/open-flags/link") == 0);
    CHECK(open("/open-flags/data", O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
    CHECK(open("/open-flags/link", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    int link = open("/open-flags/link", O_PATH | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    CHECK(link >= 0 && fstat(link, &info) == 0 && S_ISLNK(info.st_mode));
    CHECK(close(link) == 0);
    int directory = open("/open-flags", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(directory >= 0 && (fcntl(directory, F_GETFL) & O_DIRECTORY) && close(directory) == 0);
    DIR *entries = opendir("/open-flags");
    CHECK(entries);
    unsigned seen = 0;
    struct dirent *entry;
    while ((entry = readdir(entries))) {
        if (!strcmp(entry->d_name, "data")) seen |= 1;
        if (!strcmp(entry->d_name, "link")) seen |= 2;
    }
    CHECK(seen == 3 && closedir(entries) == 0);
    CHECK(unlink("/open-flags/link") == 0 && unlink("/open-flags/data") == 0 && rmdir("/open-flags") == 0);
    printf("{\"test\":\"linux_aarch64_file_open_flags\",\"passed\":true,\"libc\":\"%s\"}\n", libc);
}
