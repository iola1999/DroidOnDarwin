/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* SDK graphics clients call this public symbol through their PLT. */
int qemu_pipe_open_ns(const char *ns, const char *name, int flags)
{
    if (ns || !name || (strcmp(name, "opengles") && strcmp(name, "GLProcessPipe") && strcmp(name, "refcount"))) {
        errno = ENOENT;
        return -ENOENT;
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = "/dev/nce-render"};
    int fd = socket(AF_UNIX, SOCK_STREAM | (flags & O_CLOEXEC ? SOCK_CLOEXEC : 0), 0);
    if (fd < 0) return -errno;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -error;
    }
    char service[64];
    size_t size = (size_t)snprintf(service, sizeof(service), "pipe:%s", name) + 1;
    for (size_t offset = 0; offset < size;) {
        ssize_t count = send(fd, service + offset, size - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            int error = count < 0 ? errno : EIO;
            close(fd);
            errno = error;
            return -error;
        }
        offset += (size_t)count;
    }
    /* Blocking sockets let Linux sleep while the renderer has no reply. */
    return fd;
}
