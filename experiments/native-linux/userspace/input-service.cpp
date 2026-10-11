/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include "../input.h"
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "input-service:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(117); \
        } \
    } while (0)
static_assert(NCE_KEY_HOME == KEY_HOME && NCE_KEY_BACK == KEY_BACK && NCE_KEY_RECENT == KEY_APPSELECT);

static char event_path[128];
static pid_t input_process = -1;

static void device_node(const char *description, const char *path)
{
    unsigned major, minor;
    FILE *file = fopen(description, "r");
    CHECK(file && fscanf(file, "%u:%u", &major, &minor) == 2 && fclose(file) == 0);
    CHECK(mknod(path, S_IFCHR | 0660, makedev(major, minor)) == 0);
    CHECK(chown(path, 1000, 1004) == 0 && chmod(path, 0660) == 0);
}

static void emit(int fd, unsigned type, unsigned code, int value)
{
    input_event event = {};
    event.type = type;
    event.code = code;
    event.value = value;
    CHECK(write(fd, &event, sizeof(event)) == sizeof(event));
}

static void touch(int fd, int action, int x, int y)
{
    emit(fd, EV_ABS, ABS_X, x);
    emit(fd, EV_ABS, ABS_Y, y);
    if (action != NCE_INPUT_MOVE)
        emit(fd, EV_KEY, BTN_TOUCH, action == NCE_INPUT_DOWN);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void key(int fd, int code)
{
    emit(fd, EV_KEY, code, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    emit(fd, EV_KEY, code, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void verify_events(int fd)
{
    int reader = open(event_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    CHECK(reader >= 0);
    touch(fd, 0, 10, 20);
    touch(fd, 1, 30, 40);
    touch(fd, 2, 30, 40);
    key(fd, KEY_HOME);
    unsigned frames = 0, touches = 0, keys = 0;
    int x = -1, y = -1;
    input_event events[32];
    ssize_t bytes;
    while ((bytes = read(reader, events, sizeof(events))) > 0) {
        CHECK(bytes % sizeof(input_event) == 0);
        for (unsigned i = 0; i < bytes / sizeof(input_event); ++i) {
            const auto &event = events[i];
            if (event.type == EV_SYN && event.code == SYN_REPORT)
                ++frames;
            if (event.type == EV_ABS && event.code == ABS_X)
                x = event.value;
            if (event.type == EV_ABS && event.code == ABS_Y)
                y = event.value;
            if (event.type == EV_KEY && event.code == BTN_TOUCH) {
                CHECK(touches < 2 && event.value == (touches == 0));
                ++touches;
            }
            if (event.type == EV_KEY && event.code == KEY_HOME) {
                CHECK(keys < 2 && event.value == (keys == 0));
                ++keys;
            }
        }
    }
    CHECK(bytes == -1 && errno == EAGAIN && close(reader) == 0);
    CHECK(frames == 5 && touches == 2 && keys == 2 && x == 30 && y == 40);
    puts("{\"test\":\"linux_uinput_touch_navigation\",\"passed\":true,\"frames\":5}");
    fflush(stdout);
}

void nce_input_start()
{
    CHECK(mkdir("/dev/input", 0755) == 0);
    device_node("/sys/class/misc/uinput/dev", "/dev/uinput");
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0);
    CHECK(ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0);
    const int keys[] = {BTN_TOUCH, KEY_HOME, KEY_BACK, KEY_APPSELECT};
    for (int code : keys)
        CHECK(ioctl(fd, UI_SET_KEYBIT, code) == 0);
    CHECK(ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) == 0);
    for (unsigned axis = 0; axis < 2; ++axis) {
        int code = axis ? ABS_Y : ABS_X;
        CHECK(ioctl(fd, UI_SET_ABSBIT, code) == 0);
        uinput_abs_setup setup = {};
        setup.code = code;
        setup.absinfo.maximum = axis ? 639 : 359;
        CHECK(ioctl(fd, UI_ABS_SETUP, &setup) == 0);
    }
    uinput_setup setup = {};
    strcpy(setup.name, "NCE Touch");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 1;
    setup.id.product = 1;
    CHECK(ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0);
    char name[80], path[256];
    CHECK(ioctl(fd, UI_GET_SYSNAME(sizeof(name)), name) > 0);
    snprintf(path, sizeof(path), "/sys/class/input/%s", name);
    DIR *directory = opendir(path);
    CHECK(directory);
    while (dirent *entry = readdir(directory)) {
        if (strncmp(entry->d_name, "event", 5))
            continue;
        CHECK(!event_path[0]);
        CHECK(snprintf(event_path, sizeof(event_path), "/dev/input/%s", entry->d_name) < static_cast<int>(sizeof(event_path)));
        char description[256];
        CHECK(snprintf(description, sizeof(description), "/sys/class/input/%s/dev", entry->d_name) < static_cast<int>(sizeof(description)));
        device_node(description, event_path);
    }
    CHECK(closedir(directory) == 0 && event_path[0]);
    verify_events(fd);
    int listener = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, "/dev/nce-input");
    CHECK(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    CHECK(chmod(address.sun_path, 0600) == 0);
    input_process = fork();
    CHECK(input_process >= 0);
    if (!input_process) {
        bool pressed = false;
        for (;;) {
            NceInputCommand command;
            ssize_t count = recv(listener, &command, sizeof(command), MSG_TRUNC);
            if (count < 0 && errno == EINTR)
                continue;
            CHECK(count >= 0);
            if (count != sizeof(command))
                continue;
            int action = command.action, x = command.x, y = command.y;
            if (action >= NCE_INPUT_DOWN && action <= NCE_INPUT_UP) {
                if (x < 0 || x >= 360 || y < 0 || y >= 640)
                    continue;
                if ((action == NCE_INPUT_DOWN && pressed) || (action != NCE_INPUT_DOWN && !pressed))
                    continue;
                touch(fd, action, x, y);
                pressed = action != NCE_INPUT_UP;
            } else if (action == NCE_INPUT_KEY) {
                if (x == KEY_HOME || x == KEY_BACK || x == KEY_APPSELECT)
                    key(fd, x);
            }
        }
    }
    CHECK(close(listener) == 0 && close(fd) == 0);
}

void nce_input_stop()
{
    int status;
    CHECK(input_process > 0 && kill(input_process, SIGTERM) == 0);
    CHECK(waitpid(input_process, &status, 0) == input_process && WIFSIGNALED(status));
    CHECK(unlink(event_path) == 0 && unlink("/dev/nce-input") == 0);
    CHECK(unlink("/dev/uinput") == 0 && rmdir("/dev/input") == 0);
    event_path[0] = 0;
    input_process = -1;
}
