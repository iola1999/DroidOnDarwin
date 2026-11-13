/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include "../graphics/host-graphics.h"
#include "../graphics/graphics-socket-address.h"
#include "renderControl_opcodes.h"
#undef OP_last
#include "gles2_opcodes.h"

typedef unsigned int u32;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "gpu-test:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(117); \
        } \
    } while (0)
static int fd;
static unsigned char outgoing[65536];
static u32 outgoing_size, max_fragment = 113;
static void socket_io(void *buffer, u32 bytes, int reading)
{
    unsigned char *data = buffer;
    while (bytes) {
        u32 size = bytes > max_fragment ? max_fragment : bytes;
        ssize_t count = reading ? read(fd, data, size) : write(fd, data, size);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count > 0);
        data += count;
        bytes -= count;
    }
}
static void flush(void)
{
    socket_io(outgoing, outgoing_size, 0);
    outgoing_size = 0;
}
static void transfer(void *buffer, u32 bytes, int reading)
{
    if (reading) {
        flush();
        socket_io(buffer, bytes, 1);
        return;
    }
    const unsigned char *data = buffer;
    while (bytes) {
        u32 size = sizeof(outgoing) - outgoing_size;
        if (size > bytes)
            size = bytes;
        memcpy(outgoing + outgoing_size, data, size);
        outgoing_size += size;
        data += size;
        bytes -= size;
        if (outgoing_size == sizeof(outgoing))
            flush();
    }
}
#define COMMAND(op, ...) \
    do { \
        u32 words[] = {op, 0, ##__VA_ARGS__}; \
        words[1] = sizeof(words); \
        transfer(words, sizeof(words), 0); \
    } while (0)
static u32 response(void)
{
    u32 value;
    transfer(&value, 4, 1);
    return value;
}
static u32 float_bits(float f)
{
    u32 n;
    memcpy(&n, &f, 4);
    return n;
}
static void blob_command(u32 opcode, const u32 *prefix, u32 prefix_words,
                         const void *data, u32 bytes, const u32 *suffix, u32 suffix_words)
{
    u32 header[] = {opcode, 8 + prefix_words * 4 + bytes + suffix_words * 4};
    transfer(header, sizeof(header), 0);
    transfer((void *)prefix, prefix_words * 4, 0);
    transfer((void *)data, bytes, 0);
    transfer((void *)suffix, suffix_words * 4, 0);
}
static u32 shader(u32 kind, const char *source)
{
    COMMAND(OP_glCreateShader, kind);
    u32 id = response();
    CHECK(id);
    u32 bytes = strlen(source) + 1, prefix[] = {id, bytes};
    blob_command(OP_glShaderString, prefix, 2, source, bytes, &bytes, 1);
    COMMAND(OP_glCompileShader, id);
    COMMAND(OP_glGetShaderiv, id, 0x8b81, 4);
    CHECK(response() == 1);
    return id;
}
static void write_all(int output, const void *data, size_t bytes)
{
    const char *p = data;
    while (bytes) {
        ssize_t n = write(output, p, bytes);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0);
        p += n;
        bytes -= n;
    }
}
static void bitmap(const unsigned char *pixels)
{
    const u32 width = NCE_GPU_WIDTH, height = NCE_GPU_HEIGHT, bytes = width * height * 4;
    unsigned char header[54] = {'B', 'M'};
    u32 size = 54 + bytes, offset = 54, info = 40;
    unsigned short planes = 1, depth = 32;
    memcpy(header + 2, &size, 4);
    memcpy(header + 10, &offset, 4);
    memcpy(header + 14, &info, 4);
    memcpy(header + 18, &width, 4);
    memcpy(header + 22, &height, 4);
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &depth, 2);
    memcpy(header + 34, &bytes, 4);
    int output = open("/data/nce/gpu.bmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(output >= 0);
    write_all(output, header, sizeof(header));
    unsigned char row[NCE_GPU_WIDTH * 4];
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const unsigned char *p = pixels + (y * width + x) * 4;
            row[x * 4] = p[2];
            row[x * 4 + 1] = p[1];
            row[x * 4 + 2] = p[0];
            row[x * 4 + 3] = p[3];
        }
        write_all(output, row, sizeof(row));
    }
    CHECK(close(output) == 0);
}
static double milliseconds(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static void draw_scene(u32 surface)
{
    u32 vertex = shader(0x8b31,
                        "attribute vec2 p; varying vec2 uv; uniform float phase;"
                        "void main(){uv=p*.5+.5;gl_Position=vec4(p.x+.15*sin(phase),p.y,0.,1.);}");
    u32 fragment = shader(0x8b30,
                          "precision mediump float; varying vec2 uv;"
                          "void main(){gl_FragColor=vec4(.15+.8*uv.y,.25+.65*uv.x,.9-.6*uv.y,1.);}");
    COMMAND(OP_glCreateProgram);
    u32 program = response();
    CHECK(program);
    COMMAND(OP_glAttachShader, program, vertex);
    COMMAND(OP_glAttachShader, program, fragment);
    u32 bind[] = {program, 0, 2};
    blob_command(OP_glBindAttribLocation, bind, 3, "p", 2, NULL, 0);
    COMMAND(OP_glLinkProgram, program);
    COMMAND(OP_glGetProgramiv, program, 0x8b82, 4);
    CHECK(response() == 1);
    COMMAND(OP_glUseProgram, program);
    u32 uniform[] = {program, 6};
    blob_command(OP_glGetUniformLocation, uniform, 2, "phase", 6, NULL, 0);
    u32 phase = response();
    CHECK(phase != 0xffffffff);
    COMMAND(OP_glGenBuffers, 1, 4);
    u32 buffer = response();
    CHECK(buffer);
    COMMAND(OP_glBindBuffer, 0x8892, buffer);
    float vertices[] = {-.75f, -.6f, .75f, -.6f, 0, .7f};
    u32 buffer_prefix[] = {0x8892, sizeof(vertices), sizeof(vertices)}, usage = 0x88e4;
    blob_command(OP_glBufferData, buffer_prefix, 3, vertices, sizeof(vertices), &usage, 1);
    /* GLboolean occupies one byte in the gfxstream wire format. */
    unsigned char pointer[29] = {0};
    u32 fields[] = {OP_glVertexAttribPointerOffset, sizeof(pointer), 0, 2, 0x1406};
    memcpy(pointer, fields, sizeof(fields));
    u32 stride = 8;
    memcpy(pointer + 21, &stride, 4);
    transfer(pointer, sizeof(pointer), 0);
    COMMAND(OP_glEnableVertexAttribArray, 0);
    COMMAND(OP_glDisable, 0x0c11);
    COMMAND(OP_glViewport, 0, 0, NCE_GPU_WIDTH, NCE_GPU_HEIGHT);
    COMMAND(OP_rcCreateColorBuffer, NCE_GPU_WIDTH, NCE_GPU_HEIGHT, 0x1908);
    u32 color_buffer = response();
    CHECK(color_buffer);
    int reference = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(reference >= 0);
    struct nce_graphics_socket_address reference_address = NCE_GRAPHICS_SOCKET_ADDRESS_INIT;
    CHECK(connect(reference, (struct sockaddr *)&reference_address, NCE_GRAPHICS_SOCKET_ADDRESS_BYTES) == 0);
    write_all(reference, "pipe:refcount", sizeof("pipe:refcount"));
    write_all(reference, &color_buffer, sizeof(color_buffer));
    COMMAND(OP_rcSetWindowColorBuffer, surface, color_buffer);
    double started = milliseconds();
    const u32 frames = 120;
    for (u32 frame = 0; frame < frames; ++frame) {
        COMMAND(OP_glClearColor, float_bits(.035f), float_bits(.055f), float_bits(.09f), float_bits(1));
        COMMAND(OP_glClear, 0x4000);
        COMMAND(OP_glUniform1f, phase, float_bits(frame * .05f));
        COMMAND(OP_glDrawArrays, 4, 0, 3);
        COMMAND(OP_rcFlushWindowColorBuffer, surface);
        CHECK(response() == 0);
        COMMAND(OP_rcFBPost, color_buffer);
    }
    COMMAND(OP_glFinishRoundTrip);
    (void)response();
    double elapsed = milliseconds() - started;
    unsigned char *pixels = malloc(NCE_GPU_WIDTH * NCE_GPU_HEIGHT * 4);
    CHECK(pixels);
    COMMAND(OP_glReadPixels, 0, 0, NCE_GPU_WIDTH, NCE_GPU_HEIGHT, 0x1908, 0x1401,
            NCE_GPU_WIDTH * NCE_GPU_HEIGHT * 4);
    transfer(pixels, NCE_GPU_WIDTH * NCE_GPU_HEIGHT * 4, 1);
    const unsigned char *center = pixels + (NCE_GPU_HEIGHT / 2 * NCE_GPU_WIDTH + NCE_GPU_WIDTH / 2) * 4;
    CHECK(center[0] > 50 && center[1] > 50 && center[2] > 50 && center[3] == 255);
    CHECK(pixels[0] < 20 && pixels[1] < 30 && pixels[2] < 40 && pixels[3] == 255);
    unsigned char *posted = malloc(NCE_GPU_WIDTH * NCE_GPU_HEIGHT * 4);
    CHECK(posted);
    COMMAND(OP_rcReadColorBuffer, color_buffer, 0, 0, NCE_GPU_WIDTH, NCE_GPU_HEIGHT,
            0x1908, 0x1401, NCE_GPU_WIDTH * NCE_GPU_HEIGHT * 4);
    transfer(posted, NCE_GPU_WIDTH * NCE_GPU_HEIGHT * 4, 1);
    /* gfxstream flips the EGL pbuffer when copying into an Android color buffer. */
    unsigned max_delta = 0;
    for (u32 y = 0; y < NCE_GPU_HEIGHT; ++y)
        for (u32 x = 0; x < NCE_GPU_WIDTH * 4; ++x) {
            int delta = abs((int)pixels[y * NCE_GPU_WIDTH * 4 + x] -
                            posted[(NCE_GPU_HEIGHT - 1 - y) * NCE_GPU_WIDTH * 4 + x]);
            if ((unsigned)delta > max_delta)
                max_delta = delta;
        }
    CHECK(max_delta <= 1);
    bitmap(pixels);
    free(posted);
    free(pixels);
    printf("{\"test\":\"linux_native_gles_color_buffer\",\"passed\":true,\"pixels\":230400,\"max_channel_delta\":%u}\n", max_delta);
    COMMAND(OP_glGetError);
    CHECK(response() == 0);
    COMMAND(OP_glDisableVertexAttribArray, 0);
    COMMAND(OP_glBindBuffer, 0x8892, 0);
    COMMAND(OP_glDeleteBuffers, 1, 4, buffer);
    COMMAND(OP_glUseProgram, 0);
    COMMAND(OP_glDeleteProgram, program);
    COMMAND(OP_glDeleteShader, vertex);
    COMMAND(OP_glDeleteShader, fragment);
    COMMAND(OP_rcSetWindowColorBuffer, surface, 0);
    COMMAND(OP_rcCloseColorBuffer, color_buffer);
    COMMAND(OP_glFinishRoundTrip);
    (void)response();
    CHECK(close(reference) == 0);
    printf("{\"test\":\"linux_native_gles_shader_frames\",\"passed\":true,\"width\":%u,\"height\":%u,\"frames\":%u,\"draw_and_post_ms\":%.3f,\"ms_per_frame\":%.3f}\n",
           NCE_GPU_WIDTH, NCE_GPU_HEIGHT, frames, elapsed, elapsed / frames);
}

static void render(int scene, unsigned variant)
{
    alarm(45);
    CHECK(outgoing_size == 0);
    max_fragment = scene ? 65536 : 113;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    struct nce_graphics_socket_address address = NCE_GRAPHICS_SOCKET_ADDRESS_INIT;
    CHECK(connect(fd, (struct sockaddr *)&address, NCE_GRAPHICS_SOCKET_ADDRESS_BYTES) == 0);
    u32 flags = 0;
    transfer(&flags, 4, 0);
    COMMAND(OP_rcGetRendererVersion);
    CHECK(response() > 0);
    COMMAND(OP_rcChooseConfig, 52, 0x3040, 4, 0x3033, 1, 0x3024, 8, 0x3023, 8,
            0x3022, 8, 0x3021, 8, 0x3038, 52, 4, 1);
    u32 configs[2];
    transfer(configs, 8, 1);
    CHECK(configs[1] > 0);
    COMMAND(OP_rcCreateContext, configs[0], 0, 2);
    u32 context = response();
    CHECK(context != 0);
    COMMAND(OP_rcCreateWindowSurface, configs[0], scene ? NCE_GPU_WIDTH : 64, scene ? NCE_GPU_HEIGHT : 96);
    u32 surface = response();
    CHECK(surface != 0);
    COMMAND(OP_rcMakeCurrent, context, surface, surface);
    CHECK(response() != 0);
    COMMAND(OP_rcGetGLString, 0x1f01, 256, 256);
    char renderer[256];
    transfer(renderer, 256, 1);
    CHECK(response() > 0);
    renderer[255] = 0;
    CHECK(strstr(renderer, "ANGLE Metal Renderer"));
    printf("{\"renderer\":\"%s\"}\n", renderer);
    COMMAND(OP_glClearColor, 0x3f800000, 0, variant ? 0x3f800000 : 0, 0x3f800000);
    COMMAND(OP_glClear, 0x4000);
    COMMAND(OP_glEnable, 0x0c11);
    COMMAND(OP_glScissor, 0, 0, 32, 96);
    COMMAND(OP_glClearColor, 0, 0x3f800000, variant ? 0x3f800000 : 0, 0x3f800000);
    COMMAND(OP_glClear, 0x4000);
    COMMAND(OP_glReadPixels, 0, 0, 64, 96, 0x1908, 0x1401, 64 * 96 * 4);
    static unsigned char pixels[64 * 96 * 4];
    transfer(pixels, sizeof(pixels), 1);
    COMMAND(OP_glGetError);
    u32 error = response();
    if (error)
        dprintf(2, "GL error pid=%d context=%u error=%x\n", getpid(), context, error);
    CHECK(error == 0);
    for (u32 y = 0; y < 96; ++y)
        for (u32 x = 0; x < 64; ++x) {
            unsigned char *p = &pixels[(y * 64 + x) * 4];
            if (p[0] != (x < 32 ? 0 : 255) || p[1] != (x < 32 ? 255 : 0) || p[2] != (variant ? 255 : 0) || p[3] != 255)
                dprintf(2, "pixel mismatch pid=%d context=%u surface=%u xy=%u,%u rgba=%u,%u,%u,%u\n",
                        getpid(), context, surface, x, y, p[0], p[1], p[2], p[3]);
            CHECK(p[0] == (x < 32 ? 0 : 255) && p[1] == (x < 32 ? 255 : 0) &&
                  p[2] == (variant ? 255 : 0) && p[3] == 255);
        }
    if (scene)
        draw_scene(surface);
    COMMAND(OP_rcMakeCurrent, 0, 0, 0);
    CHECK(response() != 0);
    COMMAND(OP_rcDestroyWindowSurface, surface);
    COMMAND(OP_rcDestroyContext, context);
    COMMAND(OP_rcGetRendererVersion);
    CHECK(response() > 0);
    CHECK(close(fd) == 0);
    alarm(0);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(mkdir("/data", 0755) == 0 || errno == EEXIST);
    CHECK(mkdir("/data/nce", 0755) == 0 || errno == EEXIST);
    render(0, 0);
    render(0, 1);
    pid_t children[2];
    for (unsigned i = 0; i < 2; ++i) {
        children[i] = fork();
        CHECK(children[i] >= 0);
        if (!children[i]) {
            render(0, i);
            _exit(0);
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        int status;
        CHECK(waitpid(children[i], &status, 0) == children[i] && status == 0);
    }
    render(1, 0);
    puts("{\"test\":\"linux_native_gles_metal\",\"passed\":true,\"connections\":5,\"pixels_per_connection\":6144}");
    return 0;
}
