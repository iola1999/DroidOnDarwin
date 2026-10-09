/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dlfcn.h>
#include <jni.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
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

static double now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) _exit(12);
    return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static jobject native_identity(JNIEnv *env, jclass klass, jobject value)
{
    (void)klass;
    return (*env)->NewLocalRef(env, value);
}

static int test_jit(JNIEnv *env, jclass workload, void *library)
{
    bool (*compile)(JNIEnv *, jmethodID) = dlsym(library, "nce_art_compile_method");
    void (*keep_interpreted)(JNIEnv *, jmethodID) = dlsym(library, "nce_art_keep_interpreted");
    bool (*wait_compiled)(JNIEnv *, jmethodID) = dlsym(library, "nce_art_wait_for_compilation");
    jmethodID baseline = (*env)->GetStaticMethodID(env, workload, "mixBaseline", "(JJ)J");
    jmethodID compiled = (*env)->GetStaticMethodID(env, workload, "$compile$mix", "(JJ)J");
    if (!compile || !keep_interpreted || !wait_compiled || !baseline || !compiled) return 20;
    keep_interpreted(env, baseline);
    jmethodID interpreted = (*env)->GetStaticMethodID(env, workload, "interpretedIdentity",
                                                     "(LNceWorkload$Node;)LNceWorkload$Node;");
    if (!interpreted) return 20;
    keep_interpreted(env, interpreted);
    const jlong rounds = 2000000, seed = 123;
    uint64_t expected = seed;
    for (jlong i = 0; i < rounds; ++i) {
        expected = expected * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        expected ^= expected >> 27;
    }
    puts("{\"phase\":\"android_art_compile_method\"}");
    fflush(stdout);
    double start = now_ms();
    if (!compile(env, compiled)) return 21;
    double compile_ms = now_ms() - start;
    double interpreter_ms = 1e30, jit_ms = 1e30;
    for (unsigned i = 0; i < 3; ++i) {
        start = now_ms();
        jlong first = (*env)->CallStaticLongMethod(env, workload, baseline, rounds, seed);
        double elapsed = now_ms() - start;
        if (elapsed < interpreter_ms) interpreter_ms = elapsed;
        start = now_ms();
        jlong second = (*env)->CallStaticLongMethod(env, workload, compiled, rounds, seed);
        elapsed = now_ms() - start;
        if (elapsed < jit_ms) jit_ms = elapsed;
        if ((*env)->ExceptionCheck(env) || (uint64_t)first != expected || (uint64_t)second != expected) return 22;
    }
    printf("{\"test\":\"android_art_jit_arithmetic\",\"passed\":true,\"iterations\":%lld,"
           "\"compile_ms\":%.3f,\"interpreter_ms\":%.3f,\"jit_ms\":%.3f,\"checksum\":\"%016llx\"}\n",
           (long long)rounds, compile_ms, interpreter_ms, jit_ms, (unsigned long long)expected);
    fflush(stdout);
    const char *methods[][2] = {
        {"$compile$mutate", "([II)J"},
        {"$compile$walk", "(LNceWorkload$Node;I)J"},
        {"$compile$link", "(LNceWorkload$Node;LNceWorkload$Node;)V"},
        {"$compile$at", "([II)I"},
        {"$compile$identity", "(LNceWorkload$Node;)LNceWorkload$Node;"},
        {"$compile$callInterpreted", "(LNceWorkload$Node;)LNceWorkload$Node;"},
        {"$compile$callCompiled", "(LNceWorkload$Node;)LNceWorkload$Node;"},
        {"$compile$callNative", "(Ljava/lang/Object;)Ljava/lang/Object;"},
        {"$compile$newNode", "(I)LNceWorkload$Node;"},
        {"$compile$newArray", "(I)[I"},
        {"$compile$allocateLoop", "(I)J"},
        {"$compile$virtual", "(LNceWorkload$Value;)I"},
        {"$compile$interfaceCollision", "(LNceWorkload$CollisionReader;)I"},
        {"$compile$patternRoundTrip", "(LNceWorkload$PatternBox;Ljava/lang/Object;)Ljava/lang/Object;"},
        {"$compile$referenceStore", "([Ljava/lang/Object;ILjava/lang/Object;)Ljava/lang/Object;"},
        {"$compile$typedStore", "([LNceWorkload$Node;ILNceWorkload$Node;)V"},
        {"$compile$clearStore", "([Ljava/lang/Object;I)V"},
        {"$compile$interfaceRead", "(LNceWorkload$Reader;)I"},
        {"$compile$interfaceIdentity", "(LNceWorkload$Reader;Ljava/lang/Object;)Ljava/lang/Object;"},
        {"$compile$monitor", "(LNceWorkload$Node;Ljava/lang/Object;ILjava/lang/RuntimeException;)LNceWorkload$Node;"},
        {"$compile$catchStore", "([Ljava/lang/Object;Ljava/lang/Object;)I"},
    };
    for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); ++i) {
        jmethodID method = (*env)->GetStaticMethodID(env, workload, methods[i][0], methods[i][1]);
        if (!method || !compile(env, method)) {
            fprintf(stderr, "ART could not compile %s\n", methods[i][0]);
            return 23;
        }
    }
    jmethodID objects = (*env)->GetStaticMethodID(env, workload, "compiledObjects", "()V");
    if (!objects) return 24;
    puts("{\"phase\":\"android_art_jit_objects\"}");
    fflush(stdout);
    (*env)->CallStaticVoidMethod(env, workload, objects);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); return 25; }
    JNINativeMethod native = {"nativeIdentity", "(Ljava/lang/Object;)Ljava/lang/Object;", native_identity};
    if ((*env)->RegisterNatives(env, workload, &native, 1) != JNI_OK) return 26;
    jmethodID calls = (*env)->GetStaticMethodID(env, workload, "compiledCalls", "()Ljava/lang/Object;");
    if (!calls) return 27;
    puts("{\"phase\":\"android_art_jit_calls\"}");
    fflush(stdout);
    jobject returned = (*env)->CallStaticObjectMethod(env, workload, calls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); return 28; }
    if (!returned) return 29;
    jmethodID identity = (*env)->GetStaticMethodID(env, workload, "$compile$identity",
                                                 "(LNceWorkload$Node;)LNceWorkload$Node;");
    jobject same = (*env)->CallStaticObjectMethod(env, workload, identity, returned);
    if ((*env)->ExceptionCheck(env) || !(*env)->IsSameObject(env, same, returned)) return 30;
    (*env)->DeleteLocalRef(env, same);
    (*env)->DeleteLocalRef(env, returned);
    jmethodID allocations = (*env)->GetStaticMethodID(env, workload, "compiledAllocations", "()V");
    if (!allocations) return 35;
    (*env)->CallStaticVoidMethod(env, workload, allocations);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); return 36; }
    bool (*compile_pattern)(JNIEnv *, jmethodID) = dlsym(library, "nce_art_compile_pattern");
    jclass box = (*env)->FindClass(env, "NceWorkload$PatternBox");
    if (!compile_pattern || !box) return 41;
    jmethodID static_reference = (*env)->GetStaticMethodID(env, box, "getStaticReference", "()Ljava/lang/Object;");
    if (!static_reference) return 45;
    jobject initial = (*env)->CallStaticObjectMethod(env, box, static_reference);
    if ((*env)->ExceptionCheck(env) || initial) return 46;
    const char *patterns[][2] = {
        {"<init>", "(I)V"}, {"<init>", "(Ljava/lang/Object;)V"},
        {"getNumber", "()I"}, {"setNumber", "(I)V"},
        {"getReference", "()Ljava/lang/Object;"}, {"setReference", "(Ljava/lang/Object;)V"},
        {"getFraction", "()F"}, {"setFraction", "(F)V"},
        {"getWide", "()J"}, {"setWide", "(J)V"},
        {"getStaticReference", "()Ljava/lang/Object;"},
    };
    for (unsigned i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
        jmethodID method = i < 10
            ? (*env)->GetMethodID(env, box, patterns[i][0], patterns[i][1])
            : (*env)->GetStaticMethodID(env, box, patterns[i][0], patterns[i][1]);
        if (!method || !compile_pattern(env, method)) {
            fprintf(stderr, "ART could not install pattern %s %s\n", patterns[i][0], patterns[i][1]);
            return 42;
        }
    }
    jmethodID static_wide = (*env)->GetStaticMethodID(env, box, "getStaticWide", "()J");
    if (!static_wide || !compile(env, static_wide)) return 47;
    jmethodID pattern_test = (*env)->GetStaticMethodID(env, workload, "compiledPatterns", "()V");
    if (!pattern_test) return 43;
    (*env)->CallStaticVoidMethod(env, workload, pattern_test);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); return 44; }
    jmethodID runtime_features = (*env)->GetStaticMethodID(env, workload, "compiledRuntimeFeatures", "()V");
    if (!runtime_features) return 39;
    puts("{\"phase\":\"android_art_jit_runtime_features\"}");
    fflush(stdout);
    (*env)->CallStaticVoidMethod(env, workload, runtime_features);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); return 40; }
    jmethodID unsupported = (*env)->GetStaticMethodID(env, workload, "$compile$unsupportedCast",
                                                     "(Ljava/lang/Object;)Ljava/lang/Object;");
    if (!unsupported || compile(env, unsupported)) return 31;
    jobject fallback = (*env)->CallStaticObjectMethod(env, workload, unsupported, workload);
    if ((*env)->ExceptionCheck(env) || !(*env)->IsSameObject(env, fallback, workload)) return 37;
    (*env)->DeleteLocalRef(env, fallback);
    jmethodID hot = (*env)->GetStaticMethodID(env, workload, "automaticMix", "(I)I");
    jmethodID warm = (*env)->GetStaticMethodID(env, workload, "warmJit", "()J");
    if (!hot || !warm) return 32;
    if (wait_compiled(env, hot) || wait_compiled(env, baseline)) return 38;
    jlong sum = (*env)->CallStaticLongMethod(env, workload, warm);
    if ((*env)->ExceptionCheck(env) || sum != 60003100000LL || !wait_compiled(env, hot)) return 33;
    if (wait_compiled(env, unsupported)) return 34;
    puts("{\"test\":\"android_art_jit_automatic\",\"passed\":true,\"warmup_calls\":200000,"
         "\"unsupported_retains_interpreter\":true}");
    return 0;
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
        {.optionString = "-Xusejit:true"},
        {.optionString = "-Xjitmaxsize:32m"},
        {.optionString = "-Xjitthreshold:65535"},
        {.optionString = "-Xcompiler-option"},
        {.optionString = "--inline-max-code-units=0"},
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
    int jit_result = test_jit(env, workload, library);
    if (jit_result) return jit_result;
    if ((*vm)->DestroyJavaVM(vm) != JNI_OK) return 8;
    alarm(0);
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) return 12;
    double elapsed_ms = (end.tv_sec - begin.tv_sec) * 1000.0 +
                        (end.tv_nsec - begin.tv_nsec) / 1000000.0;
    printf("{\"test\":\"android_art_vm_destroyed\",\"passed\":true,\"elapsed_ms\":%.3f}\n", elapsed_ms);
    return 0;
}
