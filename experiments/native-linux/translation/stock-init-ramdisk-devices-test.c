/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static jmp_buf requirement_jump;
static unsigned requirement_line;
static void fixture_require(bool passed, unsigned line)
{
    if (!passed) {
        requirement_line = line;
        longjmp(requirement_jump, 1);
    }
}
#define REQUIRE(condition) fixture_require(!!(condition), __LINE__)
static long fixture_call(long number, long *args);
static long (*call)(long, long *) = fixture_call;
static void seed_entropy(void);
#include "stock-init-ramdisk.h"

struct device_case {
    const char *name, *path;
    long mknod_result, statx_result;
    unsigned mask, kind, major, minor, links;
    bool reject;
};

static const struct device_case cases[] = {
    {"new_null", "/dev/null", 0, 0, 5, 0020000, 1, 3, 1, false},
    {"existing_console", "/dev/console", -17, 0, 5, 0020000, 5, 1, 1, false},
    {"new_urandom", "/dev/urandom", 0, 0, 5, 0020000, 1, 9, 1, false},
    {"mknod_error", "/dev/console", -13, 0, 5, 0020000, 5, 1, 1, true},
    {"statx_error", "/dev/console", -17, -2, 5, 0020000, 5, 1, 1, true},
    {"missing_type_mask", "/dev/console", -17, 0, 4, 0020000, 5, 1, 1, true},
    {"missing_nlink_mask", "/dev/console", -17, 0, 1, 0020000, 5, 1, 1, true},
    {"symlink", "/dev/console", -17, 0, 5, 0120000, 5, 1, 1, true},
    {"regular_file", "/dev/console", -17, 0, 5, 0100000, 5, 1, 1, true},
    {"wrong_major", "/dev/console", -17, 0, 5, 0020000, 6, 1, 1, true},
    {"wrong_minor", "/dev/console", -17, 0, 5, 0020000, 5, 2, 1, true},
    {"wrong_nlink", "/dev/console", -17, 0, 5, 0020000, 5, 1, 2, true},
};

static struct {
    const struct device_case *test;
    const struct stock_init_ramdisk_entry *entry;
    struct stock_init_ramdisk_status status;
    unsigned mknod, statx, chmod, chown, mtime, invalid;
    long order[8], statx_flags, statx_request;
    unsigned calls;
} state;

/* 仅模拟本测试所需的 Linux syscall；没有宿主设备或文件操作。 */
static long fixture_call(long number, long *args)
{
    if (state.calls < sizeof(state.order) / sizeof(*state.order))
        state.order[state.calls] = number;
    else
        ++state.invalid;
    ++state.calls;
    if (args[0] != -100 || strcmp((const char *)args[1], state.entry->path))
        ++state.invalid;
    switch (number) {
        case 33:
            ++state.mknod;
            if (args[2] != state.entry->mode || state.entry->rdev_major >= 256 || state.entry->rdev_minor >= 256 ||
                args[3] != (long)((state.entry->rdev_major << 8) | state.entry->rdev_minor))
                ++state.invalid;
            if (!state.test->mknod_result) {
                state.status.mode = state.entry->mode;
                state.status.rdev_major = state.entry->rdev_major;
                state.status.rdev_minor = state.entry->rdev_minor;
                state.status.links = state.entry->nlink;
            }
            return state.test->mknod_result;
        case 291: {
            ++state.statx;
            state.statx_flags = args[2];
            state.statx_request = args[3];
            if (args[2] != 0x100 || (args[3] & 5) != 5)
                ++state.invalid;
            if (!state.mtime && state.test->statx_result)
                return state.test->statx_result;
            struct stock_init_ramdisk_status reply = state.status;
            reply.mask = state.mtime ? 0x7ff : state.test->mask;
            if (!(args[2] & 0x100) && (reply.mode & 0170000) == 0120000)
                reply.mode = 0020600;
            memcpy((void *)args[4], &reply, sizeof(reply));
            return 0;
        }
        case 53:
            ++state.chmod;
            if (args[2] != (long)(state.entry->mode & 07777))
                ++state.invalid;
            state.status.mode = (state.status.mode & 0170000) | args[2];
            return 0;
        case 54:
            ++state.chown;
            if (args[2] != state.entry->uid || args[3] != state.entry->gid || args[4] != 0x100)
                ++state.invalid;
            state.status.uid = args[2];
            state.status.gid = args[3];
            return 0;
        case 88: {
            ++state.mtime;
            const long *times = (const void *)args[2];
            if (times[0] != state.entry->mtime || times[1] || times[2] != state.entry->mtime || times[3] || args[3])
                ++state.invalid;
            state.status.mtime.seconds = times[2];
            state.status.mtime.nanoseconds = times[3];
            return 0;
        }
        default:
            ++state.invalid;
            return -38;
    }
}

static void seed_entropy(void)
{
    ++state.invalid;
}

static bool run_case(const struct device_case *test)
{
    unsigned index = 0, count = sizeof(stock_init_ramdisk_entries) / sizeof(*stock_init_ramdisk_entries);
    while (index < count && strcmp(stock_init_ramdisk_entries[index].path, test->path))
        ++index;
    if (index == count)
        return false;
    memset(&state, 0, sizeof(state));
    requirement_line = 0;
    state.test = test;
    state.entry = &stock_init_ramdisk_entries[index];
    state.status.mode = test->kind | 0600;
    state.status.uid = 501;
    state.status.gid = 502;
    state.status.links = test->links;
    state.status.rdev_major = test->major;
    state.status.rdev_minor = test->minor;
    state.status.inode = 42;
    state.status.mtime.seconds = 123456;
    state.status.mtime.nanoseconds = 789;
    if (!setjmp(requirement_jump)) {
        stock_init_ramdisk_device(index);
        stock_init_ramdisk_attributes(state.entry);
        stock_init_ramdisk_mtime(state.entry);
        stock_init_ramdisk_verify(index);
    }
    bool rejected = requirement_line != 0;
    bool passed = rejected == test->reject && state.mknod == 1 && !state.invalid;
    if (test->reject) {
        passed = passed && !state.chmod && !state.chown && !state.mtime &&
                 state.statx == (test->mknod_result == -17 ? 1U : 0U);
    } else {
        const long new_order[] = {33, 53, 54, 88, 291};
        const long existing_order[] = {33, 291, 53, 54, 88, 291};
        const long *order = test->mknod_result == -17 ? existing_order : new_order;
        unsigned calls = test->mknod_result == -17 ? 6 : 5;
        passed = passed && state.calls == calls && !memcmp(state.order, order, calls * sizeof(*order)) &&
                 state.chmod == 1 && state.chown == 1 && state.mtime == 1 &&
                 state.status.mode == state.entry->mode && state.status.uid == state.entry->uid && state.status.gid == state.entry->gid &&
                 state.status.mtime.seconds == state.entry->mtime && !state.status.mtime.nanoseconds && state.status.inode == 42;
    }
    printf("{\"stock_init_ramdisk_device_case\":\"%s\",\"passed\":%s,\"path\":\"%s\",\"entry\":%u,\"original_mode\":%u,\"original_uid\":%u,\"original_gid\":%u,\"original_mtime\":%u,\"original_nlink\":%u,\"original_rdev_major\":%u,\"original_rdev_minor\":%u,\"initial_mode\":%u,\"final_mode\":%u,\"rejected\":%s,\"require_line\":%u,\"mknod_calls\":%u,\"statx_calls\":%u,\"attribute_calls\":%u,\"statx_flags\":%ld,\"statx_request\":%ld,\"invalid_syscalls\":%u}\n",
           test->name, passed ? "true" : "false", state.entry->path, index + 1,
           state.entry->mode, state.entry->uid, state.entry->gid, state.entry->mtime, state.entry->nlink,
           state.entry->rdev_major, state.entry->rdev_minor, test->kind | 0600, state.status.mode,
           rejected ? "true" : "false", requirement_line, state.mknod, state.statx, state.chmod + state.chown + state.mtime,
           state.statx_flags, state.statx_request, state.invalid);
    return passed;
}

int main(void)
{
    unsigned passed = 0, count = sizeof(cases) / sizeof(*cases);
    for (unsigned i = 0; i < count; ++i)
        if (run_case(&cases[i]))
            ++passed;
    printf("{\"stock_init_ramdisk_devices_host_test\":true,\"passed\":%s,\"cases\":%u,\"passed_cases\":%u}\n",
           passed == count ? "true" : "false", count, passed);
    return passed == count ? 0 : 1;
}
