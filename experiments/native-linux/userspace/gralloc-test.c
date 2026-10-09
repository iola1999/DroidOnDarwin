/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "gralloc-test:%d: %s errno=%d EGL=%x GL=%x\n", __LINE__, #c, errno, eglGetError(), glGetError()); \
            exit(1); \
        } \
    } while (0)

static void check_cpu(AHardwareBuffer *buffer, unsigned char red, unsigned char green)
{
    AHardwareBuffer_Desc description;
    AHardwareBuffer_describe(buffer, &description);
    unsigned char *pixels;
    CHECK(AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, (void **)&pixels) == 0);
    for (unsigned y = 0; y < description.height; ++y)
        for (unsigned x = 0; x < description.width; ++x) {
            unsigned char *pixel = pixels + (y * description.stride + x) * 4;
            CHECK(pixel[0] == red && pixel[1] == green && pixel[2] == 0 && pixel[3] == 255);
        }
    CHECK(AHardwareBuffer_unlock(buffer, NULL) == 0);
}

int main(int argc, char **argv)
{
    if (!getenv("NCE_GRALLOC_DRIVER")) {
        setenv("NCE_GRALLOC_DRIVER", "1", 1);
        setenv("LD_PRELOAD", "/system/lib64/libnce-qemupipe.so:/system/lib64/libnce-framework-log.so", 1);
        setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/vendor/lib64/egl:/vendor/lib64/hw", 1);
        execv("/system/bin/gralloc-test", argv);
        return 1;
    }
    alarm(30);
    if (argc == 3 && !strcmp(argv[1], "receive")) {
        int fd = atoi(argv[2]);
        AHardwareBuffer *received;
        CHECK(AHardwareBuffer_recvHandleFromUnixSocket(fd, &received) == 0);
        check_cpu(received, 0, 255);
        AHardwareBuffer_release(received);
        CHECK(close(fd) == 0);
        puts("{\"test\":\"android_gralloc_process_transfer\",\"passed\":true}");
        return 0;
    }
    pid_t allocator = fork();
    CHECK(allocator >= 0);
    if (!allocator) {
        execl("/vendor/bin/hw/android.hardware.graphics.allocator-service.nce", "allocator", NULL);
        _exit(101);
    }
    /* AHardwareBuffer waits for the declared allocator service. */
    AHardwareBuffer_Desc description = {.width = 37, .height = 23, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT};
    AHardwareBuffer *buffer;
    CHECK(AHardwareBuffer_allocate(&description, &buffer) == 0);
    AHardwareBuffer_describe(buffer, &description);
    CHECK(description.width == 37 && description.height == 23 && description.stride >= 37);
    unsigned char *pixels;
    CHECK(AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void **)&pixels) == 0);
    for (unsigned y = 0; y < description.height; ++y)
        for (unsigned x = 0; x < description.width; ++x) {
            unsigned char *pixel = pixels + (y * description.stride + x) * 4;
            pixel[0] = 255;
            pixel[1] = 0;
            pixel[2] = 0;
            pixel[3] = 255;
        }
    CHECK(AHardwareBuffer_unlock(buffer, NULL) == 0);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(eglInitialize(display, NULL, NULL));
    EGLConfig config;
    EGLint count;
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    CHECK(eglChooseConfig(display, config_attrs, &config, 1, &count) && count == 1);
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    CHECK(context != EGL_NO_CONTEXT);
    const EGLint surface_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    CHECK(surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context));
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC native_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    PFNEGLCREATEIMAGEKHRPROC create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_image = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    CHECK(native_buffer && create_image && destroy_image && target_image);
    const EGLint image_attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR image = create_image(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, native_buffer(buffer), image_attrs);
    CHECK(image != EGL_NO_IMAGE_KHR);
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    target_image(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    unsigned char readback[37 * 23 * 4];
    glReadPixels(0, 0, 37, 23, GL_RGBA, GL_UNSIGNED_BYTE, readback);
    CHECK(glGetError() == GL_NO_ERROR);
    for (unsigned i = 0; i < 37 * 23; ++i)
        CHECK(readback[i * 4] == 255 && readback[i * 4 + 1] == 0 && readback[i * 4 + 2] == 0 && readback[i * 4 + 3] == 255);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    CHECK(glGetError() == GL_NO_ERROR);
    check_cpu(buffer, 0, 255);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    CHECK(destroy_image(display, image));
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context) && eglDestroySurface(display, surface));
    CHECK(eglTerminate(display) && eglReleaseThread());
    puts("{\"test\":\"android_gralloc_cpu_gpu_roundtrip\",\"passed\":true,\"width\":37,\"height\":23}");

    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    pid_t receiver = fork();
    CHECK(receiver >= 0);
    if (!receiver) {
        close(sockets[0]);
        char descriptor[20];
        snprintf(descriptor, sizeof(descriptor), "%d", sockets[1]);
        execl("/system/bin/gralloc-test", "gralloc-test", "receive", descriptor, NULL);
        _exit(102);
    }
    CHECK(close(sockets[1]) == 0);
    CHECK(AHardwareBuffer_sendHandleToUnixSocket(buffer, sockets[0]) == 0);
    AHardwareBuffer_release(buffer);
    CHECK(close(sockets[0]) == 0);
    int status;
    CHECK(waitpid(receiver, &status, 0) == receiver && status == 0);
    CHECK(kill(allocator, SIGTERM) == 0 && waitpid(allocator, &status, 0) == allocator);
    return 0;
}
