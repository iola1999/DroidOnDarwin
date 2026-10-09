/* SPDX-License-Identifier: GPL-2.0-or-later */
typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned short __u16;
typedef unsigned char __u8;
typedef unsigned int __u32;
typedef unsigned long long __u64;
typedef int __s32;
/* Use the upstream virtio-gpu structs with Linux's AArch64 ioctl encoding. */
#define _DRM_H_
#define DRM_COMMAND_BASE 0x40
#define DRM_IOWR(n, type) ((3UL << 30) | (sizeof(type) << 16) | ('d' << 8) | (n))
#include "virtgpu_drm.h"
#include "renderControl_opcodes.h"
#undef OP_last
#include "gles2_opcodes.h"

static s64 syscall(u64 n, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    register u64 x8 __asm__("x8") = n;
    register u64 x0 __asm__("x0") = a;
    register u64 x1 __asm__("x1") = b;
    register u64 x2 __asm__("x2") = c;
    register u64 x3 __asm__("x3") = d;
    register u64 x4 __asm__("x4") = e;
    register u64 x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3),
                                          "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}
static void print(const char *s)
{
    u64 n = 0;
    while (s[n])
        ++n;
    syscall(64, 1, (u64)s, n, 0, 0, 0);
}
__attribute__((noreturn)) static void finish(int ok)
{
    print(ok ? "HUSK-GFXSTREAM PASS\n" : "HUSK-GFXSTREAM FAIL\n");
    syscall(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
    for (;;)
        syscall(124, 0, 0, 0, 0, 0, 0);
}
static void check(int ok, const char *step)
{
    if (!ok) {
        print(step);
        print("\n");
        finish(0);
    }
}
static int fd;
static u32 handle;
static unsigned char *memory;
static void ioctl(u64 request, void *arg)
{
    check(syscall(29, fd, request, (u64)arg, 0, 0, 0) == 0, "DRM ioctl");
}
static void transfer(void *bytes, u32 size, int read)
{
    struct drm_virtgpu_3d_wait wait = {handle, 0};
    ioctl(DRM_IOCTL_VIRTGPU_WAIT, &wait);
    if (!read)
        for (u32 i = 0; i < size; ++i)
            memory[i] = ((unsigned char *)bytes)[i];
    struct drm_virtgpu_3d_transfer_to_host transfer = {0};
    transfer.bo_handle = handle;
    transfer.box.w = size;
    transfer.box.h = transfer.box.d = 1;
    ioctl(read ? DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST : DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,
          &transfer);
    ioctl(DRM_IOCTL_VIRTGPU_WAIT, &wait);
    if (read)
        for (u32 i = 0; i < size; ++i)
            ((unsigned char *)bytes)[i] = memory[i];
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

static void run(void)
{
    fd = syscall(56, -100, (u64) "/dev/dri/renderD128", 2, 0, 0, 0);
    check(fd >= 0, "open DRM render node");
    struct drm_virtgpu_resource_create resource = {0};
    resource.target = 0;
    resource.format = 64;
    resource.bind = 1 << 17;
    resource.width = resource.size = resource.stride = 1024 * 1024;
    resource.height = resource.depth = resource.array_size = 1;
    ioctl(DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &resource);
    handle = resource.bo_handle;
    struct drm_virtgpu_map map = {0};
    map.handle = handle;
    ioctl(DRM_IOCTL_VIRTGPU_MAP, &map);
    s64 mapped = syscall(222, 0, resource.size, 3, 1, fd, map.offset);
    check(mapped > 0, "mmap DRM buffer");
    memory = (unsigned char *)mapped;
    char service[] = "pipe:opengles";
    transfer(service, sizeof(service), 0);
    u32 flags = 0;
    transfer(&flags, 4, 0);
    COMMAND(OP_rcGetRendererVersion);
    check(response() > 0, "renderer version");
    COMMAND(OP_rcChooseConfig, 52, 0x3040, 4, 0x3033, 1, 0x3024, 8, 0x3023, 8,
            0x3022, 8, 0x3021, 8, 0x3038, 52, 4, 1);
    u32 configs[2];
    transfer(configs, 8, 1);
    check(configs[1] > 0, "EGL config");
    COMMAND(OP_rcCreateContext, configs[0], 0, 2);
    u32 context = response();
    check(context != 0, "GLES context");
    COMMAND(OP_rcCreateWindowSurface, configs[0], 64, 96);
    u32 surface = response();
    check(surface != 0, "GLES surface");
    COMMAND(OP_rcMakeCurrent, context, surface, surface);
    check(response() != 0, "make current");
    COMMAND(OP_rcGetGLString, 0x1f01, 256, 256);
    char renderer[256];
    transfer(renderer, 256, 1);
    check(response() > 0, "renderer name");
    renderer[255] = 0;
    print(renderer);
    print("\n");
    COMMAND(OP_glClearColor, 0x3f800000, 0, 0, 0x3f800000);
    COMMAND(OP_glClear, 0x4000);
    COMMAND(OP_glEnable, 0x0c11);
    COMMAND(OP_glScissor, 0, 0, 32, 96);
    COMMAND(OP_glClearColor, 0, 0x3f800000, 0, 0x3f800000);
    COMMAND(OP_glClear, 0x4000);
    COMMAND(OP_glReadPixels, 0, 0, 64, 96, 0x1908, 0x1401, 64 * 96 * 4);
    static unsigned char pixels[64 * 96 * 4];
    transfer(pixels, sizeof(pixels), 1);
    for (u32 y = 0; y < 96; ++y)
        for (u32 x = 0; x < 64; ++x) {
            unsigned char *p = &pixels[(y * 64 + x) * 4];
            check(p[0] == (x < 32 ? 0 : 255) && p[1] == (x < 32 ? 255 : 0) &&
                      p[2] == 0 && p[3] == 255,
                  "rendered pixels");
        }
    COMMAND(OP_glGetError);
    check(response() == 0, "GL error");
    syscall(57, fd, 0, 0, 0, 0, 0);
    finish(1);
}

void _start(void)
{
    run();
}
