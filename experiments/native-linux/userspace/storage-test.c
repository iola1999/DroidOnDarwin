/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "storage:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(100); \
        } \
    } while (0)
#define LENGTH (1024 * 1024)
static unsigned char expected[LENGTH];
static unsigned char actual[LENGTH];

int main(void)
{
    unsigned major, minor;
    FILE *device = fopen("/sys/block/ncedisk0/dev", "r");
    CHECK(device && fscanf(device, "%u:%u", &major, &minor) == 2 && fclose(device) == 0);
    CHECK(mknod("/dev/ncedisk0", S_IFBLK | 0600, makedev(major, minor)) == 0);
    CHECK(mkdir("/storage", 0700) == 0);
    CHECK(mount("/dev/ncedisk0", "/storage", "ext4", MS_NOATIME, NULL) == 0);
    unsigned generation = 0;
    int counter = open("/storage/generation", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    CHECK(counter >= 0);
    ssize_t count = read(counter, &generation, sizeof(generation));
    CHECK(count == 0 || count == sizeof(generation));
    CHECK(generation < 1000000);
    if (generation) {
        int previous = open("/storage/payload", O_RDONLY | O_CLOEXEC);
        CHECK(previous >= 0 && read(previous, actual, LENGTH) == LENGTH && close(previous) == 0);
        for (size_t i = 0; i < LENGTH; ++i)
            CHECK(actual[i] == (unsigned char)(i * 37 + (i >> 9) + generation));
    }
    ++generation;
    for (size_t i = 0; i < LENGTH; ++i)
        expected[i] = (unsigned char)(i * 37 + (i >> 9) + generation);
    int fd = open("/storage/staging", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0 && write(fd, expected, LENGTH) == LENGTH && fsync(fd) == 0);
    CHECK(fsetxattr(fd, "user.nce", "ext4", 4, 0) == 0);
    unsigned char *shared = mmap(NULL, LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char *private = mmap(NULL, LENGTH, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(shared != MAP_FAILED && private != MAP_FAILED);
    CHECK(memcmp(shared, expected, LENGTH) == 0);
    private[123] ^= 0xff;
    CHECK(shared[123] == expected[123]);
    shared[16384] ^= 0x55;
    CHECK(msync(shared, LENGTH, MS_SYNC) == 0);
    CHECK(pread(fd, actual, 1, 16384) == 1 && actual[0] == shared[16384]);
    shared[16384] = expected[16384];
    CHECK(msync(shared, LENGTH, MS_SYNC) == 0);
    CHECK(munmap(private, LENGTH) == 0 && munmap(shared, LENGTH) == 0);
    CHECK(ftruncate(fd, LENGTH + 16384) == 0);
    memset(actual, 1, 16384);
    CHECK(pread(fd, actual, 16384, LENGTH) == 16384);
    for (size_t i = 0; i < 16384; ++i)
        CHECK(actual[i] == 0);
    CHECK(ftruncate(fd, LENGTH) == 0 && fsync(fd) == 0 && close(fd) == 0);
    CHECK(rename("/storage/staging", "/storage/payload") == 0);
    CHECK(link("/storage/payload", "/storage/alias") == 0);
    CHECK(unlink("/storage/alias") == 0);
    CHECK(pwrite(counter, &generation, sizeof(generation), 0) == sizeof(generation));
    CHECK(fsync(counter) == 0 && close(counter) == 0);
    int directory = open("/storage", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory >= 0 && fsync(directory) == 0 && close(directory) == 0);
    CHECK(umount("/storage") == 0);
    CHECK(mount("/dev/ncedisk0", "/storage", "ext4", MS_RDONLY | MS_NOATIME, NULL) == 0);
    fd = open("/storage/payload", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && read(fd, actual, LENGTH) == LENGTH);
    CHECK(memcmp(actual, expected, LENGTH) == 0);
    char attribute[8];
    CHECK(fgetxattr(fd, "user.nce", attribute, sizeof(attribute)) == 4);
    CHECK(memcmp(attribute, "ext4", 4) == 0 && close(fd) == 0);
    CHECK(open("/storage/readonly", O_WRONLY | O_CREAT, 0600) == -1 && errno == EROFS);
    CHECK(umount("/storage") == 0);
    printf("{\"test\":\"linux_ext4_persistence\",\"generation\":%u,\"bytes\":%u,\"passed\":true}\n", generation, LENGTH);
    return 0;
}
