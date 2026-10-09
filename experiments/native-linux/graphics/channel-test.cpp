// SPDX-License-Identifier: GPL-2.0-or-later
#include <render-utils/render_api.h>
#include <render-utils/RenderLib.h>
#include <renderControl_opcodes.h>
#undef OP_last
#include <gles2_opcodes.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>

static void require(bool ok, const char *operation) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", operation); std::exit(1); }
}

static thread_local gfxstream::RenderChannelPtr channel;
static thread_local gfxstream::RenderChannel::Buffer pending;
static thread_local size_t pendingOffset;
static thread_local gfxstream::RenderChannel::Buffer outgoing;

static void transfer(void *data, size_t length, bool read) {
    using Channel = gfxstream::RenderChannel;
    if (!read) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        auto offset = outgoing.size();
        outgoing.resize(offset + length);
        std::memcpy(outgoing.data() + offset, bytes, length);
        return;
    }
    if (!outgoing.empty()) {
        while (true) {
            auto result = channel->tryWrite(std::move(outgoing));
            if (result == Channel::IoResult::Ok) break;
            require(result == Channel::IoResult::TryAgain, "channel write");
            channel->waitUntilWritable();
        }
        outgoing.clear();
    }
    auto *output = static_cast<unsigned char *>(data);
    while (length) {
        if (pendingOffset == pending.size()) {
            auto result = channel->tryRead(&pending);
            if (result == Channel::IoResult::TryAgain) {
                channel->waitUntilReadable();
                continue;
            }
            require(result == Channel::IoResult::Ok, "channel read");
            pendingOffset = 0;
        }
        size_t count = std::min(length, pending.size() - pendingOffset);
        std::memcpy(output, pending.data() + pendingOffset, count);
        pendingOffset += count;
        output += count;
        length -= count;
    }
}

static void command(uint32_t opcode, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> words{opcode, uint32_t(8 + args.size() * 4)};
    words.insert(words.end(), args);
    transfer(words.data(), words.size() * 4, false);
}

static uint32_t response() {
    uint32_t result = 0;
    transfer(&result, sizeof(result), true);
    return result;
}

static uint32_t bits(float value) {
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

static void render(const gfxstream::RendererPtr &renderer, bool blue = false) {
    pending.clear(); pendingOffset = 0;
    channel = renderer->createRenderChannel();
    require(bool(channel), "render channel");
    uint32_t flags = 0;
    transfer(&flags, sizeof(flags), false);
    command(OP_rcGetRendererVersion, {});
    const uint32_t version = response();
    require(version > 0, "renderer protocol version");

    // GLES 2, RGBA8 and pbuffer support. Wire pointer fields carry byte lengths.
    const uint32_t attributeBytes = 13 * 4;
    command(OP_rcChooseConfig, {attributeBytes, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE, attributeBytes, 4, 1});
    uint32_t chosen[2]{};
    transfer(chosen, sizeof(chosen), true);
    require(chosen[1] > 0, "choose EGL config");
    command(OP_rcCreateContext, {chosen[0], 0, 2});
    uint32_t context = response();
    require(context != 0, "create GLES context");
    constexpr uint32_t width = 64, height = 96;
    command(OP_rcCreateWindowSurface, {chosen[0], width, height});
    uint32_t surface = response();
    require(surface != 0, "create surface");
    command(OP_rcMakeCurrent, {context, surface, surface});
    require(response() != 0, "make current");

    command(OP_rcGetGLString, {GL_RENDERER, 256, 256});
    std::array<char, 256> rendererName{};
    transfer(rendererName.data(), rendererName.size(), true);
    require(response() > 0, "GL renderer string");
    require(std::strstr(rendererName.data(), "ANGLE Metal Renderer") != nullptr, "Metal renderer");
    command(OP_glClearColor, {bits(1), bits(0), bits(blue ? 1 : 0), bits(1)});
    command(OP_glClear, {GL_COLOR_BUFFER_BIT});
    command(OP_glEnable, {GL_SCISSOR_TEST});
    command(OP_glScissor, {0, 0, width / 2, height});
    command(OP_glClearColor, {bits(0), bits(1), bits(blue ? 1 : 0), bits(1)});
    command(OP_glClear, {GL_COLOR_BUFFER_BIT});
    command(OP_glReadPixels, {0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, width * height * 4});
    std::array<unsigned char, width * height * 4> pixels{};
    transfer(pixels.data(), pixels.size(), true);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const unsigned char *pixel = &pixels[(y * width + x) * 4];
            const bool left = x < width / 2;
            if (pixel[3] != 255) std::fprintf(stderr, "BAD context=%u surface=%u RGBA=%u,%u,%u,%u\n", context, surface, pixel[0],pixel[1],pixel[2],pixel[3]);
            require(pixel[0] == (left ? 0 : 255) && pixel[1] == (left ? 255 : 0) &&
                    pixel[2] == (blue ? 255 : 0) && pixel[3] == 255, "rendered pixel");
        }
    }
    command(OP_glGetError, {});
    require(response() == GL_NO_ERROR, "GL error state");
    command(OP_rcMakeCurrent, {0, 0, 0});
    require(response() != 0, "release current context");
    command(OP_rcDestroyWindowSurface, {surface});
    command(OP_rcDestroyContext, {context});
    command(OP_rcGetRendererVersion, {});
    require(response() == version, "drain command stream");
    channel->stop();
    channel.reset();
    std::puts("PASS: direct RenderChannel, 6144 pixels and teardown");
}

int main() {
    auto library = gfxstream::initLibrary();
    require(bool(library), "renderer library");
    library->setRenderer(SELECTED_RENDERER_HOST);
    library->setGuestAndroidApiLevel(36);
    gfxstream::host::FeatureSet features;
    features.Surfaceless.setEnabled(true);
    features.EglOnEgl.setEnabled(true);
    auto renderer = library->initRenderer(64, 96, features, false);
    require(bool(renderer), "renderer initialize");
    render(renderer); render(renderer);
    for (int round = 0; round < 100; ++round) {
        std::thread first([&] { render(renderer, false); });
        std::thread second([&] { render(renderer, true); });
        first.join(); second.join();
    }
    renderer->stop(true);
    renderer.reset(); library.reset();
    std::puts("PASS: 202 contexts, 100 parallel pairs, no pixel mismatch");
}
