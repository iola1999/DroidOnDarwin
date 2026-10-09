/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>

int main(void)
{
    void *library = dlopen("libart.so", RTLD_NOW);
    if (!library) { fprintf(stderr, "dlopen libart: %s\n", dlerror()); return 1; }
    jint (*created)(JavaVM **, jsize, jsize *) = dlsym(library, "JNI_GetCreatedJavaVMs");
    if (!created) return 2;
    jsize count = -1;
    if (created(NULL, 0, &count) != JNI_OK || count != 0) return 3;
    puts("{\"test\":\"android_art_library_loaded\",\"passed\":true}");
    return 0;
}
