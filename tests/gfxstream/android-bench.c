/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void check(int ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s: EGL=%x GL=%x\n", operation, eglGetError(), glGetError());
        exit(1);
    }
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static GLuint shader(GLenum type, const char *source)
{
    GLuint object = glCreateShader(type);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint ok = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(object, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
    }
    check(ok, "compile shader");
    return object;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const double initialized = now();
    int frames = argc > 1 ? atoi(argv[1]) : 60;
    check(frames > 0 && frames <= 1000, "frame count");
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    check(eglInitialize(display, NULL, NULL), "initialize");
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    check(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1, "config");
    const EGLint size[] = {EGL_WIDTH, 360, EGL_HEIGHT, 800, EGL_NONE};
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, version);
    check(eglMakeCurrent(display, surface, surface, context), "make current");
    printf("stage=context_ready seconds=%.6f\n", now() - initialized);
    printf("renderer=%s\n", glGetString(GL_RENDERER));
    fflush(stdout);
    GLuint vertex = shader(GL_VERTEX_SHADER,
        "attribute vec2 p; void main() { gl_Position=vec4(p,0.0,1.0); }");
    /* Repeated texture sampling models a fill-heavy pass. The input depends on
     * position and a per-frame uniform, so the compiler cannot fold the image. */
    GLuint fragment = shader(GL_FRAGMENT_SHADER,
        "precision highp float; uniform sampler2D tex; uniform float phase;"
        "void main() { vec2 uv=gl_FragCoord.xy/vec2(360.0,800.0); vec4 c=vec4(0.0);"
        "for(int i=0;i<16;++i) { c+=texture2D(tex,uv+vec2(float(i)/512.0,phase)); }"
        "gl_FragColor=c/16.0; }");
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    check(linked, "link program");
    printf("stage=program_linked seconds=%.6f\n", now() - initialized);
    glUseProgram(program);
    float triangle[] = {-1,-1,3,-1,-1,3};
    GLuint buffer, texture;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    unsigned char pixels[64 * 64 * 4];
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        int offset = (y * 64 + x) * 4;
        pixels[offset] = x * 4;
        pixels[offset + 1] = y * 4;
        pixels[offset + 2] = 64;
        pixels[offset + 3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glViewport(0, 0, 360, 800);
    GLint phase = glGetUniformLocation(program, "phase");
    /* Warm shader compilation and texture upload before timing. */
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    printf("stage=warm_complete seconds=%.6f\n", now() - initialized);
    double start = now();
    for (int frame = 0; frame < frames; ++frame) {
        glUniform1f(phase, (frame % 8) / 256.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
    }
    double elapsed = now() - start;
    unsigned char pixel[4];
    glReadPixels(180, 400, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    check(glGetError() == GL_NO_ERROR, "render");
    check(pixel[0] >= 129 && pixel[0] <= 132 && pixel[1] >= 125 && pixel[1] <= 134 &&
          pixel[2] == 64 && pixel[3] == 255, "readback pixel");
    printf("PASS frames=%d seconds=%.6f ms_per_frame=%.3f pixel=%u,%u,%u,%u\n",
           frames, elapsed, elapsed * 1000 / frames, pixel[0], pixel[1], pixel[2], pixel[3]);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return 0;
}
