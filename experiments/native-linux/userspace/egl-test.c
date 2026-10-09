/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "egl-test:%d: %s EGL=%x GL=%x\n", __LINE__, #c, eglGetError(), glGetError()); exit(1); } } while (0)

static GLuint shader(GLenum type, const char *source)
{
    GLuint object = glCreateShader(type);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint compiled;
    glGetShaderiv(object, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char message[1024];
        glGetShaderInfoLog(object, sizeof(message), NULL, message);
        fprintf(stderr, "%s\n", message);
    }
    CHECK(compiled);
    return object;
}

int main(int argc, char **argv)
{
    (void)argc;
    if (!getenv("NCE_EGL_DRIVER")) {
        setenv("NCE_EGL_DRIVER", "1", 1);
        setenv("LD_PRELOAD", "/system/lib64/libnce-qemupipe.so:/system/lib64/libnce-framework-log.so", 1);
        setenv("LD_LIBRARY_PATH", "/system/lib64:/vendor/lib64:/vendor/lib64/egl", 1);
        execv(argv[0], argv);
        perror("execv");
        return 1;
    }
    alarm(30);
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(display != EGL_NO_DISPLAY);
    EGLint major, minor, count;
    CHECK(eglInitialize(display, &major, &minor));
    fprintf(stderr, "EGL %d.%d %s\n", major, minor, eglQueryString(display, EGL_VENDOR));
    EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    CHECK(eglChooseConfig(display, config_attrs, &config, 1, &count) && count == 1);
    EGLint surface_attrs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surface_attrs);
    CHECK(surface != EGL_NO_SURFACE);
    EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    CHECK(context != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, context));
    fprintf(stderr, "GLES %s renderer %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixels[64 * 64 * 4];
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    CHECK(glGetError() == GL_NO_ERROR);
    for (int i = 0; i < 64 * 64; ++i) {
        CHECK(abs(pixels[i * 4] - 64) <= 1 && abs(pixels[i * 4 + 1] - 128) <= 1 &&
              abs(pixels[i * 4 + 2] - 191) <= 1 && pixels[i * 4 + 3] == 255);
    }
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    const unsigned char texels[] = {255, 0, 0, 255, 0, 255, 0, 255,
                                    0, 0, 255, 255, 255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "attribute vec2 position; varying vec2 uv; void main(){uv=position*0.5+0.5;gl_Position=vec4(position,0,1);}");
    GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 uv; uniform sampler2D image; void main(){gl_FragColor=texture2D(image,uv);}");
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex); glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    const float vertices[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint buffer;
    glGenBuffers(1, &buffer); glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, 64, 64);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    CHECK(glGetError() == GL_NO_ERROR);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        CHECK(!memcmp(pixels + (y * 64 + x) * 4, texels + ((y / 32) * 2 + x / 32) * 4, 4));
    puts("{\"test\":\"android_sdk_gles_shader_texture\",\"passed\":true,\"pixels\":4096}");
    glFinish();
    EGLContext shared = eglCreateContext(display, config, context, context_attrs);
    CHECK(shared != EGL_NO_CONTEXT && eglMakeCurrent(display, surface, surface, shared));
    CHECK(glIsTexture(texture));
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    unsigned char shared_pixels[16];
    glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, shared_pixels);
    CHECK(glGetError() == GL_NO_ERROR && !memcmp(texels, shared_pixels, sizeof(texels)));
    glDeleteFramebuffers(1, &framebuffer);
    CHECK(eglMakeCurrent(display, surface, surface, context) && eglDestroyContext(display, shared));
    glDeleteBuffers(1, &buffer); glDeleteTextures(1, &texture);
    glDeleteProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
    CHECK(glGetError() == GL_NO_ERROR);
    puts("{\"test\":\"android_sdk_egl_shared_texture\",\"passed\":true}");
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context) && eglDestroySurface(display, surface));
    CHECK(eglTerminate(display) && eglReleaseThread());
    puts("{\"test\":\"android_sdk_egl_driver_pixels\",\"passed\":true,\"pixels\":4096}");
    return 0;
}
