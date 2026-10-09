/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dlfcn.h>
#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

static void deadline(int signal_number)
{
    (void)signal_number;
    const char message[] = "ART Java test exceeded its 90-second deadline\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

int main(void)
{
    if (signal(SIGALRM, deadline) == SIG_ERR) return 11;
    alarm(90);
    void *library = dlopen("libart.so", RTLD_NOW);
    if (!library) { fprintf(stderr, "dlopen libart: %s\n", dlerror()); return 1; }
    jint (*created)(JavaVM **, jsize, jsize *) = dlsym(library, "JNI_GetCreatedJavaVMs");
    if (!created) return 2;
    jsize count = -1;
    if (created(NULL, 0, &count) != JNI_OK || count != 0) return 3;
    puts("{\"test\":\"android_art_library_loaded\",\"passed\":true}");
    fflush(stdout);
    void *log_library = dlopen("liblog.so", RTLD_NOW);
    if (!log_library) return 9;
    void (*set_logger)(__android_logger_function) = dlsym(log_library, "__android_log_set_logger");
    __android_logger_function stderr_logger = dlsym(log_library, "__android_log_stderr_logger");
    if (!set_logger || !stderr_logger) return 10;
    set_logger(stderr_logger);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ART_ROOT", "/apex/com.android.art", 1);
    setenv("ANDROID_I18N_ROOT", "/apex/com.android.i18n", 1);
    setenv("ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata", 1);
    setenv("ICU_DATA", "/apex/com.android.i18n/etc/icu", 1);
    jint (*create)(JavaVM **, void **, void *) = dlsym(library, "JNI_CreateJavaVM");
    if (!create) return 4;
    JavaVMOption options[] = {
        {.optionString = "-Xint"},
        {.optionString = "-Xusejit:false"},
        {.optionString = "-Xnoimage-dex2oat"},
        {.optionString = "-Ximage:/data/no-boot-image.art"},
        {.optionString = "-Xms8m"},
        {.optionString = "-Xmx64m"},
        {.optionString = "-Xbootclasspath:/apex/com.android.art/javalib/core-oj.jar:"
            "/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:"
            "/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:"
            "/apex/com.android.i18n/javalib/core-icu4j.jar:/apex/com.android.conscrypt/javalib/conscrypt.jar"},
        {.optionString = "-Djava.class.path=/data/nce/NceWorkload.dex"},
        {.optionString = "-Djava.library.path=/system/lib64"},
        {.optionString = "-verbose:gc"},
    };
    JavaVMInitArgs args = {.version = JNI_VERSION_1_6,
        .nOptions = sizeof(options) / sizeof(options[0]), .options = options};
    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) return 12;
    puts("{\"phase\":\"android_art_create_vm\"}");
    fflush(stdout);
    if (create(&vm, (void **)&env, &args) != JNI_OK || !vm || !env) return 5;
    puts("{\"phase\":\"android_art_java_workload\"}");
    fflush(stdout);
    jclass workload = (*env)->FindClass(env, "NceWorkload");
    jmethodID main_method = workload ? (*env)->GetStaticMethodID(env, workload, "main", "([Ljava/lang/String;)V") : NULL;
    if (main_method) (*env)->CallStaticVoidMethod(env, workload, main_method, NULL);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); return 6; }
    if (!main_method) return 7;
    if ((*vm)->DestroyJavaVM(vm) != JNI_OK) return 8;
    alarm(0);
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return 12;
    double elapsed_ms = (end.tv_sec - begin.tv_sec) * 1000.0 +
                        (end.tv_nsec - begin.tv_nsec) / 1000000.0;
    printf("{\"test\":\"android_art_vm_destroyed\",\"passed\":true,\"elapsed_ms\":%.3f}\n", elapsed_ms);
    return 0;
}
