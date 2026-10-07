// SPDX-License-Identifier: GPL-2.0-or-later
#include <rutabaga_gfx_ffi.h>
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

static void require(bool ok, const char *operation) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", operation); std::exit(1); }
}

static rutabaga *renderer;
static std::array<unsigned char, 1024 * 1024> backing;
static std::atomic<uint64_t> fenceSeen;

static void fenceCallback(uint64_t, const rutabaga_fence *fence) {
    fenceSeen.store(fence->fence_id);
}

static void transfer(void *data, size_t length, bool read) {
    require(length <= backing.size(), "transfer size");
    rutabaga_transfer transfer{};
    transfer.w = length;
    transfer.h = transfer.d = 1;
    if (read) {
        iovec output{data, length};
        require(rutabaga_resource_transfer_read(renderer, 1, 1, &transfer, &output) == 0,
                "pipe read");
    } else {
        std::memcpy(backing.data(), data, length);
        require(rutabaga_resource_transfer_write(renderer, 1, 1, &transfer) == 0,
                "pipe write");
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

int main() {
    rutabaga_builder builder{};
    builder.capset_mask = uint64_t(1) << RUTABAGA_CAPSET_GFXSTREAM_GLES;
    builder.wsi = RUTABAGA_WSI_SURFACELESS;
    builder.fence_cb = fenceCallback;
    builder.renderer_features = std::getenv("HUSK_GFXSTREAM_FEATURES");
    require(rutabaga_init(&builder, &renderer) == 0, "renderer init");
    require(rutabaga_context_create(renderer, 1, 0, "husk-test", 9) == 0, "context create");
    // Match VirtioGpuPipeStream's PIPE_BUFFER / R8_UNORM / VIRGL_BIND_CUSTOM.
    rutabaga_create_3d resource{0, 64, 1 << 17, uint32_t(backing.size()), 1, 1, 1, 0, 0, 0};
    require(rutabaga_resource_create_3d(renderer, 1, &resource) == 0, "resource create");
    iovec memory{backing.data(), backing.size()};
    rutabaga_iovecs memoryList{&memory, 1};
    require(rutabaga_resource_attach_backing(renderer, 1, &memoryList) == 0, "attach backing");
    require(rutabaga_context_attach_resource(renderer, 1, 1) == 0, "attach resource");
    char service[] = "pipe:opengles";
    transfer(service, sizeof(service), false);
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
    std::printf("Renderer: %s; protocol: %u\n", rendererName.data(), version);
    command(OP_glClearColor, {bits(1), bits(0), bits(0), bits(1)});
    command(OP_glClear, {GL_COLOR_BUFFER_BIT});
    command(OP_glEnable, {GL_SCISSOR_TEST});
    command(OP_glScissor, {0, 0, width / 2, height});
    command(OP_glClearColor, {bits(0), bits(1), bits(0), bits(1)});
    command(OP_glClear, {GL_COLOR_BUFFER_BIT});
    command(OP_glReadPixels, {0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, width * height * 4});
    std::array<unsigned char, width * height * 4> pixels{};
    transfer(pixels.data(), pixels.size(), true);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const unsigned char *pixel = &pixels[(y * width + x) * 4];
            const bool left = x < width / 2;
            require(pixel[0] == (left ? 0 : 255) && pixel[1] == (left ? 255 : 0) &&
                    pixel[2] == 0 && pixel[3] == 255, "rendered pixel");
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
    rutabaga_fence fence{};
    fence.fence_id = 7;
    require(rutabaga_create_fence(renderer, &fence) == 0 && fenceSeen.load() == 7,
            "fence callback");
    require(rutabaga_context_detach_resource(renderer, 1, 1) == 0, "detach resource");
    require(rutabaga_resource_unref(renderer, 1) == 0, "unref resource");
    require(rutabaga_context_destroy(renderer, 1) == 0, "destroy context");
    require(rutabaga_finish(&renderer) == 0, "renderer finish");
    std::puts("PASS: GLES wire commands, 6144 pixels, fence and teardown");
}
