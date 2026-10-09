/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "property-service:%d errno=%d: %s\n", __LINE__, errno, #c); _exit(110); } } while (0)
static int (*add_property)(const char *, unsigned, const char *, unsigned);
static int (*update_property)(prop_info *, const char *, unsigned);

static bool receive(int socket, void *buffer, size_t size)
{
    auto *bytes = static_cast<char *>(buffer);
    while (size) {
        ssize_t count = recv(socket, bytes, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        size -= count; bytes += count;
    }
    return true;
}
static bool read_string(int socket, char *value, size_t capacity)
{
    uint32_t size;
    if (!receive(socket, &size, sizeof(size)) || size >= capacity || !receive(socket, value, size)) return false;
    value[size] = 0;
    return strlen(value) == size;
}

static uint32_t set_property(const char *name, const char *value)
{
    size_t length = strlen(name), size = strlen(value);
    if (!length || name[0] == '.' || name[length - 1] == '.' || strstr(name, "..")) return PROP_ERROR_INVALID_NAME;
    for (size_t i = 0; i < length; ++i)
        if (!(isalnum(static_cast<unsigned char>(name[i])) || strchr("_.-:@", name[i]))) return PROP_ERROR_INVALID_NAME;
    if (!strncmp(name, "ctl.", 4)) return PROP_ERROR_HANDLE_CONTROL_MESSAGE;
    bool readonly = !strncmp(name, "ro.", 3);
    if (!readonly && size >= PROP_VALUE_MAX) return PROP_ERROR_INVALID_VALUE;
    const prop_info *property = __system_property_find(name);
    if (property && readonly) return PROP_ERROR_READ_ONLY_PROPERTY;
    int result = property ? update_property(const_cast<prop_info *>(property), value, size)
                          : add_property(name, length, value, size);
    return result ? PROP_ERROR_SET_FAILED : PROP_SUCCESS;
}

/* Bionic owns the shared-area format and futex serial updates. This fixture
 * supplies the property socket until Android init is part of the boot sequence. */
void nce_property_service(int ready)
{
    CHECK(mkdir("/dev/__properties__", 0711) == 0);
    CHECK(mount("tmpfs", "/dev/__properties__", "tmpfs", 0, "mode=0711,size=4m") == 0);
    int source = open("/data/nce/property_info", O_RDONLY | O_CLOEXEC);
    int destination = open("/dev/__properties__/property_info", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
    CHECK(source >= 0 && destination >= 0);
    char data[4096];
    ssize_t count;
    while ((count = read(source, data, sizeof(data))) > 0) CHECK(write(destination, data, count) == count);
    CHECK(count == 0 && close(source) == 0 && close(destination) == 0);
    auto init = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "__system_property_area_init"));
    add_property = reinterpret_cast<decltype(add_property)>(dlsym(RTLD_DEFAULT, "__system_property_add"));
    update_property = reinterpret_cast<decltype(update_property)>(dlsym(RTLD_DEFAULT, "__system_property_update"));
    CHECK(init && add_property && update_property && init() == 0);
    CHECK(set_property("ro.property_service.version", "2") == PROP_SUCCESS);
    CHECK(set_property("ro.nce.prototype", "1") == PROP_SUCCESS);
    CHECK(mkdir("/dev/socket", 0755) == 0);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, "/dev/socket/property_service");
    CHECK(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    CHECK(chmod(address.sun_path, 0666) == 0 && listen(listener, 8) == 0);
    CHECK(write(ready, "r", 1) == 1 && close(ready) == 0);
    for (;;) {
        int connection = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (connection < 0 && errno == EINTR) continue;
        CHECK(connection >= 0);
        timeval duration = {1, 0};
        CHECK(setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &duration, sizeof(duration)) == 0);
        ucred credentials;
        socklen_t size = sizeof(credentials);
        CHECK(getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0);
        uint32_t command, result;
        char name[256], value[4096];
        if (credentials.uid != 0 && credentials.uid != 1000) result = PROP_ERROR_PERMISSION_DENIED;
        else if (!receive(connection, &command, sizeof(command))) result = PROP_ERROR_READ_CMD;
        else if (command != PROP_MSG_SETPROP2) result = PROP_ERROR_INVALID_CMD;
        else if (!read_string(connection, name, sizeof(name)) || !read_string(connection, value, sizeof(value))) result = PROP_ERROR_READ_DATA;
        else result = set_property(name, value);
        ssize_t sent = send(connection, &result, sizeof(result), MSG_NOSIGNAL);
        CHECK(sent == sizeof(result) || (sent < 0 && (errno == EPIPE || errno == ECONNRESET)));
        CHECK(close(connection) == 0);
    }
}
