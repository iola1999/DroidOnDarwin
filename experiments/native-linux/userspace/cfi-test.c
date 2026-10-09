/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dlfcn.h>
#include <android/dlext.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

typedef int (*Value)(int);

static void *load(const char *name, Value *value)
{
    void *library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!library) { dprintf(2, "CFI dlopen: %s\n", dlerror()); abort(); }
    *value = (Value)dlsym(library, "nce_cfi_value");
    if (!*value) abort();
    return library;
}

int main(void)
{
    Value first, second;
    void *a = load("libnce-cfi-a.so", &first);
    void *b = load("libnce-cfi-b.so", &second);
    for (unsigned i = 0; i < 8; ++i) {
        if (first(9) != 20 || second(9) != 38) abort();
        if (dlclose(a)) abort();
        if (second(7) != 36) abort();
        a = load("libnce-cfi-a.so", &first);
        if (second(7) != 36) abort();
        if (dlclose(b)) abort();
        if (first(5) != 16) abort();
        b = load("libnce-cfi-b.so", &second);
    }
    pid_t child = fork();
    if (child < 0) abort();
    if (!child) {
        // The wrong signature must still be rejected after repeated library reloads.
        int (*wrong)(int, int) = (int (*)(int, int))dlsym(a, "nce_cfi_value");
        _exit(wrong(1, 2));
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGTRAP) abort();
    if (dlclose(a) || dlclose(b)) abort();
    const char *archive = "/system/etc/nce-native-libraries.apk";
    a = load("/system/etc/nce-native-libraries.apk!/lib/arm64-v8a/libnce-cfi-a.so", &first);
    b = load("/system/etc/nce-native-libraries.apk!/lib/arm64-v8a/libnce-cfi-b.so", &second);
    if (first(9) != 20 || second(9) != 38) abort();
    child = fork();
    if (child < 0) abort();
    if (!child) {
        if (first(13) != 24 || second(13) != 42 || dlclose(a) || dlclose(b)) _exit(1);
        a = load("/system/etc/nce-native-libraries.apk!/lib/arm64-v8a/libnce-cfi-a.so", &first);
        _exit(first(17) == 28 && !dlclose(a) ? 0 : 1);
    }
    if (waitpid(child, &status, 0) != child || status || first(19) != 30 || second(19) != 48) abort();
    if (dlclose(a) || dlclose(b)) abort();
    int fd = open(archive, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || syscall(245, fd, 65536UL, 1UL) != -1 || errno != EINVAL) abort();
    android_dlextinfo extension = {.flags = ANDROID_DLEXT_USE_LIBRARY_FD | ANDROID_DLEXT_USE_LIBRARY_FD_OFFSET,
                                 .library_fd = fd, .library_fd_offset = 0};
    if (android_dlopen_ext("invalid-zip-offset.so", RTLD_NOW, &extension) || close(fd)) abort();
    puts("{\"test\":\"android_apk_native_libraries\",\"passed\":true,\"libraries\":2,\"fork_reload\":true,\"invalid_offset_rejected\":true}");
    puts("{\"test\":\"android_cfi_library_reload\",\"passed\":true,\"reloads\":16,\"wrong_type_rejected\":true}");
    return 0;
}
