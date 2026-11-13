/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../graphics/graphics-socket-address.h"
#include "../graphics/protocol/renderControl_opcodes.h"
#ifdef NCE_GRAPHICS_PIPE_ORIGINAL
#include <dlfcn.h>
#include <qemu_pipe_bp.h>
#endif

#define DEVICE "/dev/goldfish_pipe_dprctd"
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            dprintf(2, "graphics-pipe-test:%d errno=%d: %s\n", __LINE__, errno, #condition); \
            _exit(117); \
        } \
    } while (0)

static int created_node;

static uint64_t nanoseconds(void)
{
    struct timespec value;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &value) == 0);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void passed(const char *name)
{
    printf("{\"test\":\"%s\",\"passed\":true}\n", name);
}

static void prepare_node(void)
{
    CHECK(mkdir("/sys", 0755) == 0 || errno == EEXIST);
    CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0 || errno == EBUSY);
    FILE *input = fopen("/sys/class/misc/goldfish_pipe_dprctd/dev", "r");
    CHECK(input);
    unsigned device_major, device_minor;
    CHECK(fscanf(input, "%u:%u", &device_major, &device_minor) == 2);
    CHECK(fclose(input) == 0 && device_major == 10);
    if (mknod(DEVICE, S_IFCHR | 0600, makedev(device_major, device_minor)) == 0)
        created_node = 1;
    else
        CHECK(errno == EEXIST);
    struct stat status;
    CHECK(stat(DEVICE, &status) == 0 && S_ISCHR(status.st_mode));
    CHECK(major(status.st_rdev) == device_major && minor(status.st_rdev) == device_minor);
    CHECK(chmod(DEVICE, 0600) == 0);
    printf("{\"test\":\"graphics_pipe_node\",\"passed\":true,\"major\":%u,\"minor\":%u,\"mode\":384}\n",
           device_major, device_minor);
}

static unsigned unix_sockets(void)
{
    FILE *input = fopen("/proc/net/unix", "r");
    CHECK(input);
    char line[512];
    CHECK(fgets(line, sizeof(line), input));
    unsigned count = 0;
    while (fgets(line, sizeof(line), input))
        ++count;
    CHECK(!ferror(input) && fclose(input) == 0);
    return count;
}

static void wait_unix_sockets(unsigned expected)
{
    uint64_t deadline = nanoseconds() + 2000000000;
    while (unix_sockets() != expected) {
        CHECK(nanoseconds() < deadline);
        CHECK(usleep(10000) == 0);
    }
}

static short ready(int fd, short events, int milliseconds)
{
    struct pollfd item = {.fd = fd, .events = events};
    int count;
    do {
        count = poll(&item, 1, milliseconds);
    } while (count < 0 && errno == EINTR);
    CHECK(count == 1 && !(item.revents & POLLNVAL));
    return item.revents;
}

static void scalar(int fd, void *buffer, size_t bytes, int reading)
{
#ifdef NCE_GRAPHICS_PIPE_ORIGINAL
    CHECK(bytes <= INT32_MAX);
    int result = reading ? qemu_pipe_read_fully(fd, buffer, (int)bytes) : qemu_pipe_write_fully(fd, buffer, (int)bytes);
    CHECK(result == 0);
#else
    unsigned char *cursor = buffer;
    while (bytes) {
        ssize_t count = reading ? read(fd, cursor, bytes) : write(fd, cursor, bytes);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EAGAIN) {
            CHECK(ready(fd, reading ? POLLIN : POLLOUT, 1000) & (reading ? POLLIN : POLLOUT));
            continue;
        }
        CHECK(count > 0);
        cursor += count;
        bytes -= count;
    }
#endif
}

static int service(const char *wire, int flags)
{
    CHECK(!strncmp(wire, "pipe:", 5));
#ifdef NCE_GRAPHICS_PIPE_ORIGINAL
    int fd = qemu_pipe_open_ns(NULL, wire + 5, flags);
#else
    int fd = open(DEVICE, flags);
#endif
    CHECK(fd >= 0);
#ifndef NCE_GRAPHICS_PIPE_ORIGINAL
    scalar(fd, (void *)wire, strlen(wire) + 1, 0);
#endif
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) == (flags & O_NONBLOCK));
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) == (flags & O_CLOEXEC ? FD_CLOEXEC : 0));
    return fd;
}

static void close_service(int fd)
{
#ifdef NCE_GRAPHICS_PIPE_ORIGINAL
    qemu_pipe_close(fd);
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
#else
    CHECK(close(fd) == 0);
#endif
}

static void vectors(int fd, struct iovec items[2], int reading)
{
    unsigned first = 0;
    while (first != 2) {
        ssize_t count = reading ? readv(fd, items + first, 2 - first) : writev(fd, items + first, 2 - first);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EAGAIN) {
            CHECK(ready(fd, reading ? POLLIN : POLLOUT, 1000) & (reading ? POLLIN : POLLOUT));
            continue;
        }
        CHECK(count > 0);
        size_t consumed = (size_t)count;
        while (first != 2 && consumed >= items[first].iov_len) {
            consumed -= items[first].iov_len;
            ++first;
        }
        CHECK(!consumed || first != 2);
        if (consumed) {
            items[first].iov_base = (char *)items[first].iov_base + consumed;
            items[first].iov_len -= consumed;
        }
    }
}

static uint32_t renderer_version(int fd)
{
    uint32_t command[] = {OP_rcGetRendererVersion, 8}, version;
    scalar(fd, command, sizeof(command), 0);
    scalar(fd, &version, sizeof(version), 1);
    CHECK(version > 0);
    return version;
}

static void no_listener(void)
{
    unsigned before = unix_sockets();
    for (unsigned nonblocking = 0; nonblocking != 2; ++nonblocking) {
        uint64_t started = nanoseconds();
        errno = 0;
        int fd = open(DEVICE, O_RDWR | O_CLOEXEC | (nonblocking ? O_NONBLOCK : 0));
        int error = errno;
        CHECK(fd == -1 && error == ECONNREFUSED);
        printf("{\"test\":\"graphics_pipe_no_listener\",\"passed\":true,\"nonblocking\":%s,\"errno\":%d,\"elapsed_ns\":%llu}\n",
               nonblocking ? "true" : "false", error, (unsigned long long)(nanoseconds() - started));
    }
    wait_unix_sockets(before);
}

static void backlog(void)
{
    unsigned before = unix_sockets();
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    struct nce_graphics_socket_address address = NCE_GRAPHICS_SOCKET_ADDRESS_INIT;
    CHECK(bind(listener, (struct sockaddr *)&address, NCE_GRAPHICS_SOCKET_ADDRESS_BYTES) == 0);
    CHECK(listen(listener, 0) == 0);
    int pending = open(DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    CHECK(pending >= 0);
    /* misc_open 持全局锁；backlog 满的阻塞与非阻塞 open 都必须返回 EAGAIN。 */
    for (unsigned nonblocking = 0; nonblocking != 2; ++nonblocking) {
        uint64_t started = nanoseconds();
        errno = 0;
        int fd = open(DEVICE, O_RDWR | (nonblocking ? O_NONBLOCK : 0));
        int error = errno;
        CHECK(fd == -1 && error == EAGAIN);
        printf("{\"test\":\"graphics_pipe_backlog\",\"passed\":true,\"nonblocking\":%s,\"errno\":%d,\"elapsed_ns\":%llu}\n",
               nonblocking ? "true" : "false", error, (unsigned long long)(nanoseconds() - started));
    }
    int peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    CHECK(peer >= 0 && close(pending) == 0 && close(peer) == 0);
    pending = open(DEVICE, O_RDWR | O_CLOEXEC);
    CHECK(pending >= 0);
    peer = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    CHECK(peer >= 0 && close(pending) == 0 && close(peer) == 0 && close(listener) == 0);
    wait_unix_sockets(before);
    passed("graphics_pipe_backlog_recovered");
}

static void scalar_and_refcount(unsigned baseline)
{
    int fd = service("pipe:opengles", O_RDWR | O_CLOEXEC);
    uint32_t flags = 0;
    scalar(fd, &flags, sizeof(flags), 0);
    uint32_t version = renderer_version(fd);
    printf("{\"test\":\"graphics_pipe_scalar\",\"passed\":true,\"renderer_version\":%u}\n", version);
    uint32_t create[] = {OP_rcCreateColorBuffer, 20, 16, 16, 0x1908}, handle;
    scalar(fd, create, sizeof(create), 0);
    scalar(fd, &handle, sizeof(handle), 1);
    CHECK(handle);
#ifdef NCE_GRAPHICS_PIPE_ORIGINAL
    int reference = qemu_pipe_open("refcount");
    CHECK(reference >= 0);
    CHECK((fcntl(reference, F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDWR | O_NONBLOCK));
    CHECK(fcntl(reference, F_GETFD) == 0);
    CHECK(qemu_pipe_write(reference, &handle, sizeof(handle)) == sizeof(handle));
#else
    int reference = service("pipe:refcount", O_RDWR | O_NONBLOCK);
    CHECK(write(reference, &handle, sizeof(handle)) == sizeof(handle));
#endif
    uint32_t release[] = {OP_rcCloseColorBuffer, 12, handle};
    scalar(fd, release, sizeof(release), 0);
    CHECK(renderer_version(fd) == version);
    close_service(reference);
    close_service(fd);
    wait_unix_sockets(baseline);
    printf("{\"test\":\"graphics_pipe_refcount\",\"passed\":true,\"handle\":%u,\"handle_bytes\":4}\n", handle);
}

static void nonblocking_vectors(unsigned baseline)
{
    int fd = service("pipe:opengles", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    uint32_t flags = 0, version = 0;
    scalar(fd, &flags, sizeof(flags), 0);
    CHECK(read(fd, &version, sizeof(version)) == -1 && errno == EAGAIN);
    short initial = ready(fd, POLLIN | POLLOUT, 0);
    CHECK((initial & POLLOUT) && !(initial & (POLLIN | POLLERR | POLLHUP)));
    struct iovec empty = {.iov_base = &version, .iov_len = sizeof(version)};
    CHECK(readv(fd, &empty, 1) == -1 && errno == EAGAIN);
    CHECK(read(fd, &version, 0) == 0 && write(fd, &version, 0) == 0);
    struct iovec zero = {.iov_base = &version, .iov_len = 0};
    CHECK(readv(fd, &zero, 1) == 0 && writev(fd, &zero, 1) == 0);
    passed("graphics_pipe_zero_length");
    uint32_t command[] = {OP_rcGetRendererVersion, 8};
    struct iovec output[2] = {{command, 3}, {(char *)command + 3, sizeof(command) - 3}};
    vectors(fd, output, 0);
    short response_ready = ready(fd, POLLIN, 1000);
    CHECK(response_ready & POLLIN);
    struct iovec input[2] = {{&version, 1}, {(char *)&version + 1, sizeof(version) - 1}};
    vectors(fd, input, 1);
    CHECK(version > 0);
    close_service(fd);
    wait_unix_sockets(baseline);
    printf("{\"test\":\"graphics_pipe_vectors_poll\",\"passed\":true,\"renderer_version\":%u,\"empty_errno\":%d,\"initial_poll\":%d,\"reply_poll\":%d}\n",
           version, EAGAIN, initial, response_ready);
}

static uint64_t process_lifetime(unsigned baseline, uint64_t previous)
{
    int fd = service("pipe:GLProcessPipe", O_RDWR | O_CLOEXEC);
    uint32_t confirmation = 100;
    scalar(fd, &confirmation, sizeof(confirmation), 0);
    uint64_t uid = 0;
    scalar(fd, &uid, sizeof(uid), 1);
    CHECK(uid > previous);
    wait_unix_sockets(baseline + 2);
    int duplicate = dup(fd);
    CHECK(duplicate >= 0 && fcntl(duplicate, F_GETFD) == 0);
    int flags = fcntl(fd, F_GETFL);
    CHECK(flags >= 0 && fcntl(duplicate, F_SETFL, flags | O_NONBLOCK) == 0);
    CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(duplicate, F_SETFL, flags) == 0);
    int control[2], acknowledgment[2];
    CHECK(pipe2(control, O_CLOEXEC) == 0 && pipe2(acknowledgment, O_CLOEXEC) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(close(control[1]) == 0 && close(acknowledgment[0]) == 0 && close(duplicate) == 0);
        char byte = 'h';
        CHECK(write(acknowledgment[1], &byte, 1) == 1);
        CHECK(read(control[0], &byte, 1) == 1);
        CHECK(close(fd) == 0 && close(control[0]) == 0 && close(acknowledgment[1]) == 0);
        _exit(0);
    }
    CHECK(close(control[0]) == 0 && close(acknowledgment[1]) == 0);
    char byte;
    CHECK(read(acknowledgment[0], &byte, 1) == 1 && byte == 'h');
    CHECK(close(fd) == 0 && close(duplicate) == 0);
    /* 最后一个 FD 由子进程持有，global renderer stop 尚未执行。 */
    wait_unix_sockets(baseline + 2);
    byte = 'c';
    CHECK(write(control[1], &byte, 1) == 1);
    CHECK(close(control[1]) == 0 && close(acknowledgment[0]) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    wait_unix_sockets(baseline);
    printf("{\"test\":\"graphics_pipe_process_lifetime\",\"passed\":true,\"uid\":%llu,\"uid_bytes\":8,\"unix_baseline\":%u,\"unix_held\":%u,\"unix_released\":%u}\n",
           (unsigned long long)uid, baseline, baseline + 2, baseline);
    return uid;
}

static void early_close(unsigned baseline)
{
    int fd = service("pipe:GLProcessPipe", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    uint32_t confirmation = 99;
    scalar(fd, &confirmation, sizeof(confirmation), 0);
    short closed = ready(fd, POLLIN | POLLHUP, 1000);
    CHECK(closed & POLLHUP);
    uint64_t uid;
    CHECK(read(fd, &uid, sizeof(uid)) == -1 && errno == EIO);
    struct iovec input = {.iov_base = &uid, .iov_len = sizeof(uid)};
    CHECK(readv(fd, &input, 1) == -1 && errno == EIO);
#ifdef NCE_GRAPHICS_PIPE_ORIGINAL
    CHECK(qemu_pipe_read_fully(fd, &uid, sizeof(uid)) == -EIO && errno == EIO);
    CHECK(qemu_pipe_read(fd, &uid, sizeof(uid)) == -EIO && errno == EIO);
    CHECK(qemu_pipe_try_again(-EIO) == 0);
    errno = EAGAIN;
    CHECK(qemu_pipe_try_again(-EAGAIN) == 1);
    errno = EINTR;
    CHECK(qemu_pipe_try_again(-EINTR) == 1);
    passed("graphics_pipe_original_eof_retry");
#endif
    CHECK(write(fd, &confirmation, sizeof(confirmation)) == -1 && errno == EPIPE);
    close_service(fd);
    wait_unix_sockets(baseline);
    printf("{\"test\":\"graphics_pipe_early_close\",\"passed\":true,\"confirmation\":99,\"read_errno\":%d,\"readv_errno\":%d,\"write_errno\":%d,\"poll\":%d}\n",
           EIO, EIO, EPIPE, closed);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(20);
    char mode[32] = {0};
    int input = open("/graphics-pipe-mode", O_RDONLY | O_CLOEXEC);
    CHECK(input >= 0 && read(input, mode, sizeof(mode) - 1) > 0 && close(input) == 0);
#ifdef NCE_GRAPHICS_PIPE_ORIGINAL
    CHECK(!strcmp(mode, "original"));
    Dl_info object;
    CHECK(dladdr((void *)qemu_pipe_open_ns, &object) != 0 && object.dli_fbase && object.dli_fname);
    CHECK(!strcmp(object.dli_fname, "/vendor/lib64/libqemupipe.ranchu.so"));
    passed("graphics_pipe_original_library");
#endif
    prepare_node();
    if (!strcmp(mode, "no-listener"))
        no_listener();
    else if (!strcmp(mode, "backlog"))
        backlog();
    else {
        CHECK(!strcmp(mode, "protocol") || !strcmp(mode, "original"));
        unsigned baseline = unix_sockets();
        CHECK(baseline > 0);
        scalar_and_refcount(baseline);
        nonblocking_vectors(baseline);
        uint64_t uid = process_lifetime(baseline, 0);
        (void)process_lifetime(baseline, uid);
        early_close(baseline);
        wait_unix_sockets(baseline);
    }
    if (created_node)
        CHECK(unlink(DEVICE) == 0);
    alarm(0);
    printf("{\"test\":\"graphics_pipe_guest_cleanup\",\"passed\":true,\"mode\":\"%s\"}\n", mode);
    return 0;
}
