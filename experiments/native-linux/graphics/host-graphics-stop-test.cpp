/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host-graphics.h"
#include "graphics-socket-address.h"
#include <render-utils/RenderLib.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dispatch/dispatch.h>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

template <typename Predicate>
static void wait_for(Predicate ready)
{
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!ready() && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(ready());
}

/* listener 使用固定结果；连接的 read/write/shutdown/close 使用真实 socketpair。 */
struct SocketFixture {
    static constexpr long initial_fd = 8000, listener_fd = 10000;
    int server, peer;
    std::atomic<bool> pending, body_read{false}, shut_down{false};
    std::atomic<unsigned> initial_closes{0}, listener_closes{0}, server_closes{0}, read_calls{0};
    std::atomic<size_t> received{0};
    size_t handshake_bytes = 0;

    explicit SocketFixture(bool accept)
    {
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        server = sockets[0];
        peer = sockets[1];
        int enabled = 1;
        assert(setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
        assert(setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
        pending.store(accept);
    }
    ~SocketFixture()
    {
        if (!server_closes.load())
            assert(close(server) == 0);
        assert(close(peer) == 0);
    }
    void send(const void *data, size_t bytes)
    {
        const auto *buffer = static_cast<const unsigned char *>(data);
        for (size_t offset = 0; offset < bytes;) {
            ssize_t written = ::send(peer, buffer + offset, bytes - offset, 0);
            if (written < 0 && errno == EINTR)
                continue;
            assert(written > 0);
            offset += written;
        }
    }
    long syscall(long number, long *args)
    {
        switch (number) {
            case 198:
                assert(args[0] == 1 && args[1] == (1 | 0x80000) && args[2] == 0);
                return initial_fd;
            case 25:
                assert(args[0] == initial_fd && args[1] == 1030 && args[2] == 16);
                return listener_fd;
            case 57:
                if (args[0] == initial_fd) {
                    ++initial_closes;
                    return 0;
                }
                if (args[0] == listener_fd) {
                    ++listener_closes;
                    return 0;
                }
                assert(args[0] == server && ++server_closes == 1);
                return close(server) == 0 ? 0 : -errno;
            case 200: {
                const unsigned char expected[] = {1, 0, 0, 'n', 'c', 'e', '-', 'r', 'e', 'n', 'd', 'e', 'r'};
                assert(args[0] == listener_fd && args[2] == sizeof(expected));
                assert(!memcmp(reinterpret_cast<void *>(args[1]), expected, sizeof(expected)));
                return 0;
            }
            case 201:
                assert(args[0] == listener_fd && args[1] == 16);
                return 0;
            case 73: {
                assert(args[1] == 1);
                struct Poll {
                    int fd;
                    short events, revents;
                };
                auto *poll = reinterpret_cast<Poll *>(args[0]);
                assert(poll->fd == listener_fd && poll->events == 1);
                poll->revents = pending.load() ? 1 : 0;
                if (!poll->revents)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return poll->revents ? 1 : 0;
            }
            case 242:
                assert(args[0] == listener_fd && args[3] == 0x80000);
                return pending.exchange(false) ? server : -4;
            case 63: {
                assert(args[0] == server);
                ++read_calls;
                if (handshake_bytes && received.load() >= handshake_bytes && args[2] == 65536)
                    body_read.store(true);
                ssize_t bytes = read(server, reinterpret_cast<void *>(args[1]), args[2]);
                if (bytes > 0)
                    received.fetch_add(bytes);
                return bytes >= 0 ? bytes : -errno;
            }
            case 64: {
                assert(args[0] == server);
                ssize_t bytes = write(server, reinterpret_cast<void *>(args[1]), args[2]);
                return bytes >= 0 ? bytes : -errno;
            }
            case 210:
                assert(args[1] == 2);
                if (args[0] == listener_fd)
                    return 0;
                assert(args[0] == server);
                shut_down.store(true);
                return shutdown(server, SHUT_RDWR) == 0 ? 0 : -errno;
            default:
                assert(!"unexpected Linux callback");
                return -38;
        }
    }
};

static std::atomic<SocketFixture *> active_socket;

static long linux_socket_fixture(long number, long *args)
{
    auto *fixture = active_socket.load();
    assert(fixture);
    return fixture->syscall(number, args);
}

namespace controlled
{
using Channel = gfxstream::RenderChannel;

/* 满队列和创建阶段的停留由回归控制，不执行图形命令。 */
class ControlledChannel final : public Channel
{
   public:
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool stopped = false, write_waiting = false, read_waiting = false, hold_flags = false;
    unsigned write_calls = 0, stop_calls = 0;

    void setEventCallback(EventCallback &&) override
    {
        assert(false);
    }
    void setWantedEvents(State) override
    {
        assert(false);
    }
    State state() const override
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped ? State::Stopped : State::Empty;
    }
    IoResult tryWrite(Buffer &&) override
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++write_calls;
        condition.notify_all();
        if (hold_flags)
            condition.wait(lock, [this] { return stopped; });
        return stopped ? IoResult::Error : IoResult::TryAgain;
    }
    void waitUntilWritable() override
    {
        std::unique_lock<std::mutex> lock(mutex);
        write_waiting = true;
        condition.notify_all();
        condition.wait(lock, [this] { return stopped; });
    }
    IoResult tryRead(Buffer *) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped ? IoResult::Error : IoResult::TryAgain;
    }
    IoResult readBefore(Buffer *, Duration) override
    {
        assert(false);
        return IoResult::Error;
    }
    void waitUntilReadable() override
    {
        std::unique_lock<std::mutex> lock(mutex);
        read_waiting = true;
        condition.notify_all();
        condition.wait(lock, [this] { return stopped; });
    }
    void stop() override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++stop_calls;
        stopped = true;
        condition.notify_all();
    }
    void onSave(gfxstream::Stream *) override
    {
        assert(false);
    }
};

struct ControlledRenderer {
    std::shared_ptr<ControlledChannel> created = std::make_shared<ControlledChannel>();
    std::mutex mutex;
    std::condition_variable condition;
    bool hold_creation = false, creation_entered = false, release_creation = false;

    gfxstream::RenderChannelPtr createRenderChannel()
    {
        std::unique_lock<std::mutex> lock(mutex);
        creation_entered = true;
        condition.notify_all();
        if (hold_creation)
            condition.wait(lock, [this] { return release_creation; });
        return created;
    }
    void onGuestGraphicsProcessCreate(uint64_t)
    {
        assert(false);
    }
    void cleanupProcGLObjects(uint64_t)
    {
        assert(false);
    }
};

struct ControlledLibrary {
    gfxstream::OnLastColorBufferRef getOnLastColorBufferRef()
    {
        return [](uint32_t) { assert(false); };
    }
};

static ControlledRenderer *renderer;
static ControlledLibrary *library;
static unsigned opened, closed;
static std::atomic<uint64_t> next_process{1};

static long call(long number, long a = 0, long b = 0, long c = 0, long d = 0)
{
    long args[6] = {a, b, c, d};
    return linux_socket_fixture(number, args);
}

#define CHECK(value) assert(value)
/* 脚本从当前生产文件提取完整 Connection；旧版本用于有界失败复现。 */
#include "connection-under-test.h"
#undef CHECK

static void run(const char *name)
{
    static const char service[] = "pipe:opengles";
    SocketFixture socket(false);
    socket.handshake_bytes = sizeof(service);
    active_socket.store(&socket);
    ControlledRenderer control;
    ControlledLibrary control_library;
    renderer = &control;
    library = &control_library;
    opened = closed = 0;
    bool creating = !strcmp(name, "creation-during-stop"), raw = !strcmp(name, "raw-flags-stop");
    control.hold_creation = creating;
    control.created->hold_flags = raw;
    auto connection = std::make_unique<Connection>(socket.server);
    if (raw) {
        const uint32_t flags = 0;
        socket.send(&flags, sizeof(flags));
        wait_for([&] {
            std::lock_guard<std::mutex> lock(control.created->mutex);
            return control.created->write_calls == 1;
        });
    } else {
        socket.send(service, sizeof(service));
        if (creating) {
            wait_for([&] {
                std::lock_guard<std::mutex> lock(control.mutex);
                return control.creation_entered;
            });
        } else {
            static const char packet[] = "full-channel";
            socket.send(packet, sizeof(packet));
            wait_for([&] {
                std::lock_guard<std::mutex> lock(control.created->mutex);
                return control.created->write_waiting && control.created->read_waiting;
            });
            std::puts("{\"phase\":\"controlled_channel_waits\",\"write\":true,\"read\":true}");
            assert(fflush(stdout) == 0);
        }
    }
    const auto started = Clock::now();
    if (creating) {
        std::thread stopper([&] { connection.reset(); });
        wait_for([&] { return socket.shut_down.load(); });
        {
            std::lock_guard<std::mutex> lock(control.mutex);
            control.release_creation = true;
            control.condition.notify_all();
        }
        stopper.join();
    } else {
        connection.reset();
    }
    assert(Clock::now() - started < std::chrono::seconds(2));
    assert(opened == 1 && closed == 1 && socket.server_closes == 1);
    assert(control.created->stopped && control.created->stop_calls >= 1);
    if (creating)
        assert(control.created->write_calls == 0 && !socket.body_read.load());
    active_socket.store(nullptr);
}
}  // namespace controlled

static void real_renderer(const char *name)
{
    const bool zero = !strcmp(name, "zero-connections");
    const unsigned cycles = zero ? 2 : 1;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        SocketFixture socket(!zero);
        static const char service[] = "pipe:opengles";
        socket.handshake_bytes = sizeof(service);
        active_socket.store(&socket);
        assert(nce_graphics_start(linux_socket_fixture) == 0);
        if (!zero) {
            if (!strcmp(name, "incomplete-handshake")) {
                socket.send("pi", 2);
                wait_for([&] { return socket.read_calls.load() >= 2; });
            } else {
                assert(!strcmp(name, "real-idle-channel"));
                socket.send(service, sizeof(service));
                wait_for([&] { return socket.body_read.load(); });
            }
        }
        const auto started = Clock::now();
        nce_graphics_stop();
        assert(Clock::now() - started < std::chrono::seconds(2));
        assert(socket.initial_closes == 1 && socket.listener_closes == 1);
        assert(socket.server_closes == (zero ? 0U : 1U));
        active_socket.store(nullptr);
    }
}

static void run_case(const char *name)
{
    if (!strcmp(name, "full-channel") || !strcmp(name, "creation-during-stop") || !strcmp(name, "raw-flags-stop"))
        controlled::run(name);
    else
        real_renderer(name);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    struct rlimit cores = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &cores) == 0);
    if (!strcmp(argv[1], "cli")) {
        run_case(argv[2]);
    } else {
        assert(!strcmp(argv[1], "gcd"));
        struct Task {
            const char *name;
            dispatch_semaphore_t completed;
        } task = {argv[2], dispatch_semaphore_create(0)};
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), &task, [](void *context) {
            auto *task = static_cast<Task *>(context);
            run_case(task->name);
            dispatch_semaphore_signal(task->completed);
        });
        assert(dispatch_semaphore_wait(task.completed, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
        dispatch_release(task.completed);
    }
    std::printf("{\"test\":\"host_graphics_stop\",\"entry\":\"%s\",\"case\":\"%s\",\"passed\":true}\n", argv[1], argv[2]);
    return fflush(stdout) == 0 ? 0 : 1;
}
