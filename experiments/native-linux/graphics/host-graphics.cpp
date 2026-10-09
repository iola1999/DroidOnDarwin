/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host-graphics.h"
#include <render-utils/render_api.h>
#include <render-utils/RenderLib.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using Channel = gfxstream::RenderChannel;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::fprintf(stderr, "host-graphics:%d: %s\n", __LINE__, #c); \
            std::abort(); \
        } \
    } while (0)
static long (*linux_call)(long, long *);
static gfxstream::RenderLibPtr library;
static gfxstream::RendererPtr renderer;
static long listener = -1;
static std::atomic<bool> stopping;
static std::thread accept_thread;
static unsigned opened, closed;
static std::atomic<uint64_t> next_process{1};
static void *display_window;

void nce_graphics_set_window(void *layer)
{
    CHECK(!renderer);
    display_window = layer;
}

static long call(long number, long a = 0, long b = 0, long c = 0, long d = 0)
{
    long args[6] = {a, b, c, d};
    return linux_call(number, args);
}

struct Connection {
    long fd;
    gfxstream::RenderChannelPtr channel;
    std::thread reader;
    std::atomic<bool> done{false};

    explicit Connection(long socket) : fd(socket)
    {
        ++opened;
        reader = std::thread([this] { run(); });
    }
    ~Connection()
    {
        call(210, fd, 2);
        reader.join();
        CHECK(call(57, fd) == 0);
        ++closed;
    }
    void output()
    {
        Channel::Buffer packet;
        for (;;) {
            auto result = channel->tryRead(&packet);
            if (result == Channel::IoResult::TryAgain) {
                channel->waitUntilReadable();
                continue;
            }
            if (result != Channel::IoResult::Ok)
                break;
            size_t offset = 0;
            while (offset < packet.size()) {
                long count = call(64, fd, (long)(packet.data() + offset), packet.size() - offset);
                if (count == -4)
                    continue;
                if (count <= 0)
                    return;
                offset += count;
            }
        }
    }
    bool transfer(void *data, size_t length, bool write)
    {
        size_t offset = 0;
        while (offset < length) {
            long count = call(write ? 64 : 63, fd, (long)((char *)data + offset), length - offset);
            if (count == -4)
                continue;
            if (count <= 0)
                return false;
            offset += count;
        }
        return true;
    }
    bool connect()
    {
        char service[128] = {};
        if (!transfer(service, 4, false))
            return false;
        bool raw = !memcmp(service, "\0\0\0\0", 4);
        if (!raw) {
            size_t size = 4;
            while (size < sizeof(service) && service[size - 1]) {
                if (!transfer(service + size, 1, false))
                    return false;
                ++size;
            }
            if (service[size - 1])
                return false;
            if (!strcmp(service, "pipe:refcount")) {
                uint32_t handle;
                if (!transfer(&handle, sizeof(handle), false))
                    return false;
                char byte;
                long count;
                do {
                    count = call(63, fd, (long)&byte, 1);
                } while (count == -4);
                library->getOnLastColorBufferRef()(handle);
                return false;
            }
            if (!strcmp(service, "pipe:GLProcessPipe")) {
                int confirmation;
                if (!transfer(&confirmation, sizeof(confirmation), false) || confirmation != 100)
                    return false;
                uint64_t id = next_process.fetch_add(1);
                renderer->onGuestGraphicsProcessCreate(id);
                if (transfer(&id, sizeof(id), true)) {
                    char byte;
                    /* The descriptor lifetime owns this guest process's GL resources. */
                    while (call(63, fd, (long)&byte, 1) == -4) {
                    }
                }
                renderer->cleanupProcGLObjects(id);
                return false;
            }
            if (strcmp(service, "pipe:opengles")) {
                std::fprintf(stderr, "Unsupported graphics pipe: %s\n", service);
                return false;
            }
        }
        channel = renderer->createRenderChannel();
        CHECK(channel);
        if (raw) {
            Channel::Buffer flags;
            flags.resize(4);
            memset(flags.data(), 0, 4);
            CHECK(channel->tryWrite(std::move(flags)) == Channel::IoResult::Ok);
        }
        return true;
    }
    void run()
    {
        if (!connect()) {
            call(210, fd, 2);
            done.store(true);
            return;
        }
        std::thread writer([this] { output(); call(210, fd, 2); });
        for (;;) {
            Channel::Buffer packet;
            packet.resize(65536);
            long count = call(63, fd, (long)packet.data(), packet.size());
            if (count == -4)
                continue;
            if (count <= 0)
                break;
            packet.resize(count);
            Channel::IoResult result;
            while ((result = channel->tryWrite(std::move(packet))) == Channel::IoResult::TryAgain)
                channel->waitUntilWritable();
            if (result != Channel::IoResult::Ok)
                break;
        }
        channel->stop();
        call(210, fd, 2);
        writer.join();
        done.store(true);
    }
};

int nce_graphics_start(long (*linux_syscall)(long, long *))
{
    CHECK(listener == -1 && !renderer);
    linux_call = linux_syscall;
    library = gfxstream::initLibrary();
    CHECK(library);
    library->setLogger([](gfxstream_logging_level, const char *file, int line, const char *, const char *message) {
        std::fprintf(stderr, "gfxstream %s:%d: %s\n", file, line, message);
    });
    library->setRenderer(SELECTED_RENDERER_HOST);
    library->setGuestAndroidApiLevel(36);
    gfxstream::host::FeatureSet features;
    features.Surfaceless.setEnabled(true);
    features.EglOnEgl.setEnabled(true);
    features.RefCountPipe.setEnabled(true);
    renderer = library->initRenderer(NCE_GPU_WIDTH, NCE_GPU_HEIGHT, features, display_window != nullptr);
    CHECK(renderer);
    if (display_window) {
        CHECK(renderer->showOpenGLSubwindow(display_window, 0, 0, NCE_GPU_WIDTH, NCE_GPU_HEIGHT,
                                            NCE_GPU_WIDTH, NCE_GPU_HEIGHT, 1, 0, false, false));
        std::puts("{\"test\":\"native_metal_display_created\",\"passed\":true}");
    }
    /* Linux sockaddr_un has a 16-bit family; Darwin's layout also has a length byte. */
    struct {
        unsigned short family;
        char path[108];
    } address = {1, "/dev/nce-render"};
    listener = call(198, 1, 1 | 0x80000, 0);
    CHECK(listener >= 0);
    /* The launcher replaces descriptors 0..2 for each guest program. */
    long reserved = call(25, listener, 1030, 16);
    CHECK(reserved >= 16 && call(57, listener) == 0);
    listener = reserved;
    CHECK(listener >= 0 && call(200, listener, (long)&address, sizeof(address)) == 0);
    CHECK(call(53, -100, (long)address.path, 0666) == 0);
    CHECK(call(201, listener, 16) == 0);
    stopping.store(false);
    opened = closed = 0;
    accept_thread = std::thread([] {
        std::vector<std::unique_ptr<Connection>> connections;
        while (!stopping.load()) {
            struct {
                int fd;
                short events, revents;
            } pollfd = {(int)listener, 1, 0};
            struct {
                long seconds, nanoseconds;
            } interval = {0, 100000000};
            long ready = call(73, (long)&pollfd, 1, (long)&interval, 0);
            if (ready == -4)
                continue;
            CHECK(ready >= 0);
            if (stopping.load())
                break;
            if (ready && (pollfd.revents & 1)) {
                long fd = call(242, listener, 0, 0, 0x80000);
                if (fd >= 0)
                    connections.push_back(std::make_unique<Connection>(fd));
                else
                    CHECK(fd == -4 || stopping.load());
            }
            for (auto it = connections.begin(); it != connections.end();) {
                if ((*it)->done.load())
                    it = connections.erase(it);
                else
                    ++it;
            }
        }
    });
    return 0;
}

void nce_graphics_stop()
{
    CHECK(listener >= 0);
    stopping.store(true);
    call(210, listener, 2);
    accept_thread.join();
    CHECK(call(57, listener) == 0);
    CHECK(call(35, -100, (long)"/dev/nce-render", 0) == 0);
    listener = -1;
    renderer->stop(true);
    renderer.reset();
    library.reset();
    CHECK(opened > 0 && opened == closed);
    std::printf("{\"test\":\"native_graphics_channels_released\",\"passed\":true,\"opened\":%u,\"closed\":%u}\n", opened, closed);
}
