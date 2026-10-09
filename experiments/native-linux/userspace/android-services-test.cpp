/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_parcel.h>
#include <android/binder_process.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binderfs.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "android-services:%d pid=%d errno=%d: %s\n", __LINE__, getpid(), errno, #c); _exit(100); } } while (0)
static constexpr unsigned kRounds = 32;
static constexpr const char *kService = "nce.test";
static bool died;
static void deadline(int) { dprintf(2, "android-services deadline exceeded\n"); _exit(124); }
void nce_property_service(int ready);
void nce_framework_test();
void nce_zygote_test();
void nce_crypto_tests();

static void wait_success(pid_t child)
{
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    if (status) dprintf(2, "android-services child=%d status=%d\n", child, status);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void load_service_policy()
{
    pid_t compiler = fork();
    CHECK(compiler >= 0);
    if (!compiler) {
        execl("/system/bin/secilc", "secilc", "/data/nce/nce-policy.cil", "-o", "/data/nce/sepolicy",
              "-f", "/data/nce/file_contexts", "-c", "33", nullptr);
        _exit(104);
    }
    wait_success(compiler);
    int input = open("/data/nce/sepolicy", O_RDONLY | O_CLOEXEC);
    CHECK(input >= 0);
    struct stat info;
    CHECK(fstat(input, &info) == 0 && info.st_size > 0);
    void *policy = mmap(nullptr, info.st_size, PROT_READ, MAP_PRIVATE, input, 0);
    CHECK(policy != MAP_FAILED);
    int load = open("/sys/fs/selinux/load", O_WRONLY | O_CLOEXEC);
    CHECK(load >= 0 && write(load, policy, info.st_size) == info.st_size && close(load) == 0);
    CHECK(munmap(policy, info.st_size) == 0 && close(input) == 0);
    int enforce = open("/sys/fs/selinux/enforce", O_RDWR | O_CLOEXEC);
    CHECK(enforce >= 0 && write(enforce, "1", 1) == 1);
    char value;
    CHECK(pread(enforce, &value, 1, 0) == 1 && value == '1' && close(enforce) == 0);
    int index = open("/sys/fs/selinux/class/service_manager/index", O_RDONLY | O_CLOEXEC);
    CHECK(index >= 0);
    char text[32] = {};
    CHECK(read(index, text, sizeof(text) - 1) > 0 && close(index) == 0);
    unsigned short class_value = 0;
    CHECK(sscanf(text, "%hu", &class_value) == 1 && class_value > 0);
    DIR *permissions = opendir("/sys/fs/selinux/class/service_manager/perms");
    CHECK(permissions);
    unsigned permission_count = 0;
    for (dirent *entry; (entry = readdir(permissions));) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        int permission = openat(dirfd(permissions), entry->d_name, O_RDONLY | O_CLOEXEC);
        CHECK(permission >= 0);
        memset(text, 0, sizeof(text));
        CHECK(read(permission, text, sizeof(text) - 1) > 0 && close(permission) == 0);
        unsigned number;
        CHECK(sscanf(text, "%u", &number) == 1 && number > 0 && number <= 3);
        ++permission_count;
    }
    CHECK(permission_count == 3 && closedir(permissions) == 0);
    puts("{\"test\":\"linux_selinux_service_policy\",\"passed\":true,\"enforcing\":true}");
}

static void unix_sockets()
{
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0);
    int enabled = 1;
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(sockets[0]);
        int fd = memfd_create("nce-unix", MFD_CLOEXEC);
        CHECK(fd >= 0 && write(fd, "unix-fd", 7) == 7);
        char byte = 'x';
        iovec vector = {&byte, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        msghdr message = {};
        message.msg_iov = &vector; message.msg_iovlen = 1;
        message.msg_control = control; message.msg_controllen = sizeof(control);
        cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(fd));
        memcpy(CMSG_DATA(header), &fd, sizeof(fd));
        CHECK(sendmsg(sockets[1], &message, 0) == 1);
        close(fd); close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]);
    char byte;
    iovec vector = {&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(ucred))] = {};
    msghdr message = {};
    message.msg_iov = &vector; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    CHECK(recvmsg(sockets[0], &message, MSG_CMSG_CLOEXEC) == 1 && byte == 'x' && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
    int received = -1;
    bool credentials = false;
    for (cmsghdr *h = CMSG_FIRSTHDR(&message); h; h = CMSG_NXTHDR(&message, h)) {
        CHECK(h->cmsg_level == SOL_SOCKET);
        if (h->cmsg_type == SCM_RIGHTS) memcpy(&received, CMSG_DATA(h), sizeof(received));
        else if (h->cmsg_type == SCM_CREDENTIALS) {
            ucred value;
            memcpy(&value, CMSG_DATA(h), sizeof(value));
            CHECK(value.pid == child && value.uid == getuid() && value.gid == getgid());
            credentials = true;
        } else CHECK(false);
    }
    char data[8] = {};
    CHECK(received >= 0 && credentials && fcntl(received, F_GETFD) == FD_CLOEXEC);
    CHECK(pread(received, data, 7, 0) == 7 && !strcmp(data, "unix-fd"));
    CHECK(close(received) == 0 && close(sockets[0]) == 0);
    wait_success(child);
    puts("{\"test\":\"linux_unix_socket_credentials_fd\",\"passed\":true}");
}

struct Payload { int8_t bytes[65536]; int32_t size; };
static bool allocate_bytes(void *opaque, int32_t length, int8_t **buffer)
{
    auto *payload = static_cast<Payload *>(opaque);
    if (length < 0 || length > static_cast<int32_t>(sizeof(payload->bytes))) return false;
    payload->size = length;
    *buffer = payload->bytes;
    return true;
}
static void *on_create(void *value) { return value; }
static void on_destroy(void *) {}
static binder_status_t on_transact(AIBinder *, transaction_code_t code, const AParcel *in, AParcel *out)
{
    CHECK(AIBinder_getCallingPid() > 0 && AIBinder_getCallingUid() == getuid());
    if (code == 1) {
        int32_t sequence;
        Payload payload;
        CHECK(AParcel_readInt32(in, &sequence) == STATUS_OK);
        CHECK(AParcel_readByteArray(in, &payload, allocate_bytes) == STATUS_OK);
        int32_t sum = 0;
        for (int32_t i = 0; i < payload.size; ++i) {
            CHECK(static_cast<uint8_t>(payload.bytes[i]) == static_cast<uint8_t>(sequence * 7 + i));
            sum += static_cast<uint8_t>(payload.bytes[i]);
        }
        CHECK(AParcel_writeInt32(out, sequence) == STATUS_OK);
        CHECK(AParcel_writeInt32(out, sum) == STATUS_OK);
        return STATUS_OK;
    }
    if (code == 2) {
        int fd;
        CHECK(AParcel_readParcelFileDescriptor(in, &fd) == STATUS_OK && fd >= 0);
        CHECK(pwrite(fd, "platform-binder", 15, 0) == 15 && close(fd) == 0);
        return AParcel_writeInt32(out, 15);
    }
    return STATUS_UNKNOWN_TRANSACTION;
}
static AIBinder_Class *binder_class()
{
    AIBinder_Class *value = AIBinder_Class_define("org.nce.IBinderTest", on_create, on_destroy, on_transact);
    CHECK(value);
    return value;
}
static void server(int ready)
{
    CHECK(ABinderProcess_setThreadPoolMaxThreadCount(1));
    AIBinder *binder = AIBinder_new(binder_class(), nullptr);
    CHECK(binder);
    CHECK(AServiceManager_addService(binder, "nce.denied") == EX_SECURITY);
    binder_exception_t result = AServiceManager_addService(binder, kService);
    dprintf(2, "android-services addService=%d\n", result);
    CHECK(result == EX_NONE);
    CHECK(write(ready, "r", 1) == 1 && close(ready) == 0);
    ABinderProcess_joinThreadPool();
    CHECK(false);
}
static void binder_died(void *) { died = true; }

static uint32_t property_request(uint32_t command, const char *name, uint32_t name_size,
                                 const char *value, uint32_t value_size)
{
    int connection = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(connection >= 0);
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, "/dev/socket/property_service");
    CHECK(connect(connection, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    iovec vectors[] = {{&command, sizeof(command)}, {&name_size, sizeof(name_size)},
                      {const_cast<char *>(name), name_size}, {&value_size, sizeof(value_size)},
                      {const_cast<char *>(value), value_size}};
    msghdr message = {};
    message.msg_iov = vectors; message.msg_iovlen = 5;
    CHECK(sendmsg(connection, &message, MSG_NOSIGNAL) == 12 + name_size + value_size);
    uint32_t result;
    CHECK(recv(connection, &result, sizeof(result), MSG_WAITALL) == sizeof(result));
    CHECK(close(connection) == 0);
    return result;
}

static void property_boundaries()
{
    CHECK(property_request(0, "", 0, "", 0) == PROP_ERROR_INVALID_CMD);
    CHECK(property_request(PROP_MSG_SETPROP2, ".bad", 4, "x", 1) == PROP_ERROR_INVALID_NAME);
    CHECK(property_request(PROP_MSG_SETPROP2, "a\0b", 3, "x", 1) == PROP_ERROR_READ_DATA);
    char name[256]; memset(name, 'a', sizeof(name));
    CHECK(property_request(PROP_MSG_SETPROP2, name, sizeof(name), "x", 1) == PROP_ERROR_READ_DATA);
    char value[PROP_VALUE_MAX]; memset(value, 'x', sizeof(value));
    CHECK(property_request(PROP_MSG_SETPROP2, "nce.mutable", 11, value, sizeof(value)) == PROP_ERROR_INVALID_VALUE);
    CHECK(property_request(PROP_MSG_SETPROP2, "ctl.start", 9, "x", 1) == PROP_ERROR_HANDLE_CONTROL_MESSAGE);
    const prop_info *property = __system_property_find("nce.mutable");
    CHECK(property);
    uint32_t serial = __system_property_serial(property), updated = 0;
    timespec immediate = {};
    CHECK(!__system_property_wait(property, serial, &updated, &immediate));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        timespec delay = {0, 10000000};
        CHECK(nanosleep(&delay, nullptr) == 0);
        CHECK(__system_property_set("nce.mutable", "notified") == 0);
        _exit(0);
    }
    timespec limit = {2, 0};
    CHECK(__system_property_wait(property, serial, &updated, &limit) && updated != serial);
    wait_success(child);
    char actual[PROP_VALUE_MAX];
    CHECK(__system_property_get("nce.mutable", actual) == 8 && !strcmp(actual, "notified"));
    puts("{\"test\":\"android_property_protocol_wait\",\"passed\":true,\"rejected_requests\":6}");
}

int main(int argc, char **argv)
{
    CHECK(signal(SIGALRM, deadline) != SIG_ERR);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "server")) { server(atoi(argv[2])); return 1; }
    if (argc == 3 && !strcmp(argv[1], "properties")) { nce_property_service(atoi(argv[2])); return 1; }
    alarm(20);
    unix_sockets();
    load_service_policy();
    int properties_ready[2];
    CHECK(pipe(properties_ready) == 0);
    pid_t properties = fork();
    CHECK(properties >= 0);
    if (!properties) {
        close(properties_ready[0]);
        char descriptor[24]; snprintf(descriptor, sizeof(descriptor), "%d", properties_ready[1]);
        execl(argv[0], argv[0], "properties", descriptor, nullptr); _exit(103);
    }
    close(properties_ready[1]);
    char properties_byte;
    CHECK(read(properties_ready[0], &properties_byte, 1) == 1 && properties_byte == 'r');
    CHECK(close(properties_ready[0]) == 0);
    auto properties_init = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "__system_properties_init"));
    CHECK(properties_init && properties_init() == 0);
    char property_value[PROP_VALUE_MAX];
    CHECK(__system_property_get("ro.property_service.version", property_value) == 1 && !strcmp(property_value, "2"));
    CHECK(__system_property_set("nce.mutable", "first") == 0);
    CHECK(__system_property_get("nce.mutable", property_value) == 5 && !strcmp(property_value, "first"));
    CHECK(__system_property_set("nce.mutable", "second") == 0);
    CHECK(__system_property_get("nce.mutable", property_value) == 6 && !strcmp(property_value, "second"));
    CHECK(__system_property_set("ro.nce.prototype", "2") == -1);
    CHECK(__system_property_get("ro.nce.prototype", property_value) == 1 && !strcmp(property_value, "1"));
    puts("{\"test\":\"android_property_shared_area\",\"passed\":true}");
    property_boundaries();
    CHECK(mkdir("/binderfs", 0700) == 0);
    CHECK(mount(nullptr, "/binderfs", "binder", 0, nullptr) == 0);
    int control = open("/binderfs/binder-control", O_RDWR | O_CLOEXEC);
    CHECK(control >= 0);
    const char *devices[] = {"nce-platform", "nce-hardware"};
    for (const char *name : devices) {
        binderfs_device device = {};
        strcpy(device.name, name);
        CHECK(ioctl(control, BINDER_CTL_ADD, &device) == 0);
        char path[128];
        snprintf(path, sizeof(path), "/binderfs/%s", name);
        CHECK(chmod(path, 0666) == 0);
    }
    CHECK(close(control) == 0);
    CHECK(symlink("/binderfs/nce-platform", "/dev/binder") == 0);
    CHECK(symlink("/binderfs/nce-hardware", "/dev/hwbinder") == 0);
    pid_t manager = fork();
    CHECK(manager >= 0);
    if (!manager) { execl("/system/bin/servicemanager", "servicemanager", nullptr); _exit(101); }
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t service = fork();
    CHECK(service >= 0);
    if (!service) {
        close(ready[0]);
        char descriptor[24]; snprintf(descriptor, sizeof(descriptor), "%d", ready[1]);
        execl(argv[0], argv[0], "server", descriptor, nullptr); _exit(102);
    }
    close(ready[1]);
    char value;
    ssize_t received = read(ready[0], &value, 1);
    if (received != 1) { int status; CHECK(waitpid(service, &status, 0) == service); dprintf(2, "service failed status=%d\n", status); }
    CHECK(received == 1 && value == 'r' && close(ready[0]) == 0);
    puts("{\"test\":\"android_platform_servicemanager_registration\",\"passed\":true}");
    AIBinder *binder = AServiceManager_checkService(kService);
    CHECK(binder && AIBinder_isRemote(binder) && AIBinder_associateClass(binder, binder_class()));
    CHECK(AServiceManager_checkService("nce.denied") == nullptr);
    const int32_t sizes[] = {0, 128, 16380, 65536};
    for (unsigned round = 0; round < kRounds; ++round) {
        AParcel *in = nullptr, *out = nullptr;
        CHECK(AIBinder_prepareTransaction(binder, &in) == STATUS_OK);
        Payload payload;
        payload.size = sizes[round % 4];
        int32_t sum = 0;
        for (int32_t i = 0; i < payload.size; ++i) {
            payload.bytes[i] = static_cast<int8_t>(round * 7 + i);
            sum += static_cast<uint8_t>(payload.bytes[i]);
        }
        CHECK(AParcel_writeInt32(in, round) == STATUS_OK);
        CHECK(AParcel_writeByteArray(in, payload.bytes, payload.size) == STATUS_OK);
        CHECK(AIBinder_transact(binder, 1, &in, &out, 0) == STATUS_OK);
        int32_t actual_sequence, actual_sum;
        CHECK(AParcel_readInt32(out, &actual_sequence) == STATUS_OK && actual_sequence == static_cast<int32_t>(round));
        CHECK(AParcel_readInt32(out, &actual_sum) == STATUS_OK && actual_sum == sum);
        AParcel_delete(out);
    }
    puts("{\"test\":\"android_platform_binder_roundtrip\",\"passed\":true,\"rounds\":32,\"max_payload\":65536}");
    int transferred = memfd_create("nce-binder", MFD_CLOEXEC);
    CHECK(transferred >= 0);
    AParcel *in = nullptr, *out = nullptr;
    CHECK(AIBinder_prepareTransaction(binder, &in) == STATUS_OK);
    CHECK(AParcel_writeParcelFileDescriptor(in, transferred) == STATUS_OK);
    CHECK(AIBinder_transact(binder, 2, &in, &out, 0) == STATUS_OK);
    int32_t written;
    CHECK(AParcel_readInt32(out, &written) == STATUS_OK && written == 15);
    AParcel_delete(out);
    char data[16] = {};
    CHECK(pread(transferred, data, 15, 0) == 15 && !strcmp(data, "platform-binder"));
    CHECK(close(transferred) == 0);
    puts("{\"test\":\"android_platform_binder_fd_transfer\",\"passed\":true}");
    alarm(60);
    nce_crypto_tests();
    puts("{\"phase\":\"android_framework_launch\"}");
    nce_framework_test();
    puts("{\"phase\":\"android_zygote_launch\"}");
    nce_zygote_test();
    alarm(20);
    int binder_fd;
    CHECK(ABinderProcess_setupPolling(&binder_fd) == STATUS_OK && binder_fd >= 0);
    AIBinder_DeathRecipient *recipient = AIBinder_DeathRecipient_new(binder_died);
    CHECK(recipient && AIBinder_linkToDeath(binder, recipient, nullptr) == STATUS_OK);
    CHECK(kill(service, SIGTERM) == 0);
    int status;
    CHECK(waitpid(service, &status, 0) == service && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    while (!died) {
        pollfd fd = {binder_fd, POLLIN, 0};
        CHECK(poll(&fd, 1, 2000) == 1 && (fd.revents & POLLIN));
        CHECK(ABinderProcess_handlePolledCommands() == STATUS_OK);
    }
    CHECK(!AIBinder_isAlive(binder));
    AIBinder_DeathRecipient_delete(recipient);
    AIBinder_decStrong(binder);
    CHECK(kill(manager, SIGTERM) == 0);
    CHECK(waitpid(manager, &status, 0) == manager && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(unlink("/dev/binder") == 0 && unlink("/binderfs/nce-platform") == 0);
    CHECK(unlink("/dev/hwbinder") == 0 && unlink("/binderfs/nce-hardware") == 0);
    /* ProcessState retains the driver fd until this client process exits. */
    CHECK(umount2("/binderfs", MNT_DETACH) == 0 && rmdir("/binderfs") == 0);
    CHECK(kill(properties, SIGTERM) == 0);
    CHECK(waitpid(properties, &status, 0) == properties && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(unlink("/dev/socket/property_service") == 0 && rmdir("/dev/socket") == 0);
    CHECK(umount2("/dev/__properties__", MNT_DETACH) == 0 && rmdir("/dev/__properties__") == 0);
    alarm(0);
    puts("{\"test\":\"android_platform_binder_cleanup\",\"passed\":true,\"death_notifications\":1}");
    return 0;
}
