/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <linux/android/binderfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "binder:%d pid=%d errno=%d: %s\n", __LINE__, getpid(), errno, #c); _exit(100); } } while (0)
#define MAPPING_SIZE (1024 * 1024)
#define ROUNDS 32
struct endpoint { int fd; unsigned char *mapping; };
struct message { uint32_t sequence, length; unsigned char bytes[65536]; };
struct reply { uint32_t sequence, length, sum; };

static struct endpoint endpoint(void)
{
    struct endpoint e = {.fd = open("/binderfs/nce", O_RDWR | O_CLOEXEC)};
    CHECK(e.fd >= 0);
    struct binder_version version = {0};
    CHECK(ioctl(e.fd, BINDER_VERSION, &version) == 0 && version.protocol_version == 8);
    CHECK(ioctl(e.fd, BINDER_VERSION, (void *)1) == -1 && errno == EINVAL);
    CHECK(ioctl(e.fd, BINDER_WRITE_READ, (void *)1) == -1 && errno == EFAULT);
    e.mapping = mmap(NULL, MAPPING_SIZE, PROT_READ, MAP_PRIVATE, e.fd, 0);
    CHECK(e.mapping != MAP_FAILED);
    return e;
}
static void close_endpoint(struct endpoint e)
{
    CHECK(munmap(e.mapping, MAPPING_SIZE) == 0 && close(e.fd) == 0);
}
static void command(int fd, uint32_t command, const void *data, size_t size)
{
    unsigned char buffer[4 + sizeof(struct binder_transaction_data)];
    CHECK(size <= sizeof(buffer) - 4);
    memcpy(buffer, &command, 4);
    if (size) memcpy(buffer + 4, data, size);
    struct binder_write_read wr = {.write_buffer = (uintptr_t)buffer, .write_size = size + 4};
    CHECK(ioctl(fd, BINDER_WRITE_READ, &wr) == 0 && wr.write_consumed == wr.write_size);
}
static uint32_t receive(struct endpoint e, struct binder_transaction_data *transaction)
{
    for (;;) {
        unsigned char buffer[512];
        struct binder_write_read wr = {.read_buffer = (uintptr_t)buffer, .read_size = sizeof(buffer)};
        CHECK(ioctl(e.fd, BINDER_WRITE_READ, &wr) == 0 && wr.read_consumed <= sizeof(buffer));
        uint32_t event = 0;
        for (size_t offset = 0; offset < wr.read_consumed;) {
            uint32_t code;
            CHECK(wr.read_consumed - offset >= 4);
            memcpy(&code, buffer + offset, 4);
            offset += 4;
            size_t size = _IOC_SIZE(code);
            CHECK(size <= wr.read_consumed - offset);
            if (code == BR_TRANSACTION || code == BR_REPLY) {
                CHECK(!event && size == sizeof(*transaction));
                memcpy(transaction, buffer + offset, size);
                uintptr_t first = transaction->data.ptr.buffer;
                CHECK(first >= (uintptr_t)e.mapping && first <= (uintptr_t)e.mapping + MAPPING_SIZE);
                CHECK(transaction->data_size <= (uintptr_t)e.mapping + MAPPING_SIZE - first);
                event = code;
            } else if (code == BR_FAILED_REPLY || code == BR_DEAD_REPLY) {
                CHECK(!event);
                struct binder_extended_error error = {0};
                CHECK(ioctl(e.fd, BINDER_GET_EXTENDED_ERROR, &error) == 0);
                dprintf(2, "binder reply code=%x command=%x error=%d\n", code, error.command, error.param);
                event = code;
            } else CHECK(code == BR_NOOP || code == BR_TRANSACTION_COMPLETE || code == BR_SPAWN_LOOPER);
            offset += size;
        }
        if (event) return event;
    }
}
static void release_buffer(struct endpoint e, struct binder_transaction_data *t)
{
    command(e.fd, BC_FREE_BUFFER, &t->data.ptr.buffer, sizeof(t->data.ptr.buffer));
}
static void server(int ready, pid_t client)
{
    struct endpoint e = endpoint();
    struct flat_binder_object manager = {.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS};
    CHECK(ioctl(e.fd, BINDER_SET_CONTEXT_MGR_EXT, &manager) == 0);
    command(e.fd, BC_ENTER_LOOPER, NULL, 0);
    CHECK(write(ready, "r", 1) == 1 && close(ready) == 0);
    for (unsigned round = 0; round <= ROUNDS; ++round) {
        struct binder_transaction_data request;
        CHECK(receive(e, &request) == BR_TRANSACTION && request.sender_pid == client);
        struct reply reply = {.sequence = round};
        if (round < ROUNDS) {
            const struct message *message = (void *)(uintptr_t)request.data.ptr.buffer;
            CHECK(request.code == 1 && request.data_size >= 8);
            CHECK(message->sequence == round && message->length <= sizeof(message->bytes));
            CHECK(request.data_size == 8 + message->length && !request.offsets_size);
            reply.length = message->length;
            for (unsigned i = 0; i < message->length; ++i) {
                CHECK(message->bytes[i] == (unsigned char)(round * 7 + i));
                reply.sum += message->bytes[i];
            }
        } else {
            CHECK(request.code == 2 && request.data_size == sizeof(struct binder_fd_object));
            CHECK(request.offsets_size == sizeof(binder_size_t));
            const struct binder_fd_object *object = (void *)(uintptr_t)request.data.ptr.buffer;
            CHECK(object->hdr.type == BINDER_TYPE_FD);
            CHECK(pwrite(object->fd, "binder-fd", 9, 0) == 9 && close(object->fd) == 0);
        }
        struct binder_transaction_data response = {.data_size = sizeof(reply), .data.ptr.buffer = (uintptr_t)&reply};
        command(e.fd, BC_REPLY, &response, sizeof(response));
        release_buffer(e, &request);
    }
    close_endpoint(e);
    _exit(0);
}
int main(void)
{
    CHECK(mkdir("/binderfs", 0700) == 0);
    CHECK(mount(NULL, "/binderfs", "binder", 0, NULL) == 0);
    int control = open("/binderfs/binder-control", O_RDWR | O_CLOEXEC);
    CHECK(control >= 0);
    struct binderfs_device device = {.name = "nce"};
    CHECK(ioctl(control, BINDER_CTL_ADD, &device) == 0 && device.major > 0);
    CHECK(close(control) == 0);
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t client = getpid(), child = fork();
    CHECK(child >= 0);
    if (!child) { close(ready[0]); server(ready[1], client); }
    CHECK(close(ready[1]) == 0);
    char value;
    CHECK(read(ready[0], &value, 1) == 1 && close(ready[0]) == 0);
    struct endpoint e = endpoint();
    struct message *message = malloc(sizeof(*message));
    CHECK(message);
    const unsigned sizes[] = {0, 128, 16380, 65536};
    for (unsigned round = 0; round < ROUNDS; ++round) {
        message->sequence = round;
        message->length = sizes[round % 4];
        unsigned sum = 0;
        for (unsigned i = 0; i < message->length; ++i) sum += message->bytes[i] = (unsigned char)(round * 7 + i);
        struct binder_transaction_data t = {.code = 1, .data_size = 8 + message->length, .data.ptr.buffer = (uintptr_t)message};
        command(e.fd, BC_TRANSACTION, &t, sizeof(t));
        CHECK(receive(e, &t) == BR_REPLY && t.data_size == sizeof(struct reply));
        const struct reply *reply = (void *)(uintptr_t)t.data.ptr.buffer;
        CHECK(reply->sequence == round && reply->length == message->length && reply->sum == sum);
        release_buffer(e, &t);
    }
    free(message);
    puts("{\"test\":\"linux_binder_cross_process_roundtrip\",\"passed\":true,\"rounds\":32,\"max_payload\":65536}");
    int transferred = open("/binder-fd", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(transferred >= 0);
    struct binder_fd_object object = {.hdr.type = BINDER_TYPE_FD, .fd = transferred};
    binder_size_t offset = 0;
    struct binder_transaction_data t = {.code = 2, .data_size = sizeof(object), .offsets_size = sizeof(offset),
        .data.ptr = {.buffer = (uintptr_t)&object, .offsets = (uintptr_t)&offset}};
    command(e.fd, BC_TRANSACTION, &t, sizeof(t));
    CHECK(receive(e, &t) == BR_REPLY && t.data_size == sizeof(struct reply));
    CHECK(((struct reply *)(uintptr_t)t.data.ptr.buffer)->sequence == ROUNDS);
    release_buffer(e, &t);
    char content[10] = {0};
    CHECK(pread(transferred, content, 9, 0) == 9 && !strcmp(content, "binder-fd"));
    CHECK(close(transferred) == 0 && unlink("/binder-fd") == 0);
    puts("{\"test\":\"linux_binder_fd_transfer\",\"passed\":true}");
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    memset(&t, 0, sizeof(t));
    t.target.handle = 0x7fffffff;
    command(e.fd, BC_TRANSACTION, &t, sizeof(t));
    CHECK(receive(e, &t) == BR_FAILED_REPLY);
    close_endpoint(e);
    CHECK(unlink("/binderfs/nce") == 0 && umount("/binderfs") == 0 && rmdir("/binderfs") == 0);
    puts("{\"test\":\"linux_binder_invalid_handle_cleanup\",\"passed\":true}");
    return 0;
}
