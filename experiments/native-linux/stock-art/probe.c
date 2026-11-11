/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#define TOKEN INT64_C(0x1122334455667788)
#define CHECK(env, name, condition) \
    do { \
        if (!(condition)) \
            fail(env, name, __LINE__); \
    } while (0)

static _Noreturn void fail(JNIEnv *env, const char *name, int line)
{
    int error = errno;
    if (env && (*env)->ExceptionCheck(env))
        (*env)->ExceptionDescribe(env);
    fprintf(stderr, "stock-art:%s:%d errno=%d\n", name, line, error);
    printf("{\"stock_art_check\":\"%s\",\"passed\":false,\"line\":%d,\"errno\":%d}\n", name, line, error);
    fflush(stdout);
    _Exit(1);
}

static void passed(const char *name, int64_t value)
{
    printf("{\"stock_art_check\":\"%s\",\"passed\":true,\"observed\":%" PRId64 "}\n", name, value);
}

static void no_exception(JNIEnv *env, const char *name)
{
    CHECK(env, name, !(*env)->ExceptionCheck(env));
}

static void snapshot(const char *name)
{
    char path[128];
    int length = snprintf(path, sizeof(path), "/data/stock-art/maps-%s.txt", name);
    CHECK(NULL, "maps", length > 0 && (size_t)length < sizeof(path));
    FILE *input = fopen("/proc/self/maps", "r"), *output = fopen(path, "w");
    CHECK(NULL, "maps", input && output);
    char data[4096];
    size_t count, total = 0;
    while ((count = fread(data, 1, sizeof(data), input))) {
        CHECK(NULL, "maps", fwrite(data, 1, count, output) == count);
        total += count;
    }
    CHECK(NULL, "maps", !ferror(input) && total > 0);
    CHECK(NULL, "maps", fclose(input) == 0 && fclose(output) == 0);
    printf("{\"stock_art_maps\":\"%s\",\"bytes\":%zu}\n", name, total);
}

static jobject native_identity(JNIEnv *env, jclass type, jobject object)
{
    (void)type;
    return (*env)->NewLocalRef(env, object);
}

static jlong native_values(JNIEnv *env, jclass type, jboolean z, jbyte b, jchar c,
                           jshort s, jint i, jlong j, jfloat f, jdouble d, jobject object)
{
    (void)type;
    CHECK(env, "native_arguments", z == JNI_TRUE && b == -101 && c == 0x4e2d && s == -12345 && i == 0x12345678 && j == TOKEN && f == 1.25f && d == -123.5 && object);
    const char *text = (*env)->GetStringUTFChars(env, (jstring)object, NULL);
    CHECK(env, "native_arguments", text && strcmp(text, "jni-token") == 0);
    (*env)->ReleaseStringUTFChars(env, (jstring)object, text);
    return j;
}

static void native_throw(JNIEnv *env, jclass type)
{
    (void)type;
    jclass exception = (*env)->FindClass(env, "java/lang/IllegalStateException");
    CHECK(env, "native_exception", exception && (*env)->ThrowNew(env, exception, "jni-native") == JNI_OK);
    (*env)->DeleteLocalRef(env, exception);
}

static jmethodID method(JNIEnv *env, jclass type, const char *name, const char *signature)
{
    jmethodID result = (*env)->GetStaticMethodID(env, type, name, signature);
    CHECK(env, name, result);
    return result;
}

struct AttachedThread {
    JavaVM *vm;
    jclass type;
    jint result;
};

static void *attached_thread(void *opaque)
{
    struct AttachedThread *thread = opaque;
    JNIEnv *env = NULL;
    CHECK(NULL, "native_thread", (*thread->vm)->GetEnv(thread->vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED);
    JavaVMAttachArgs args = {.version = JNI_VERSION_1_6, .name = "stock-art-native"};
    CHECK(NULL, "native_thread", (*thread->vm)->AttachCurrentThread(thread->vm, &env, &args) == JNI_OK && env);
    JNIEnv *same = NULL;
    CHECK(env, "native_thread", (*thread->vm)->GetEnv(thread->vm, (void **)&same, JNI_VERSION_1_6) == JNI_OK && same == env);
    jmethodID worker = method(env, thread->type, "worker", "(I)I");
    jvalue argument = {.i = 17};
    thread->result = (*env)->CallStaticIntMethodA(env, thread->type, worker, &argument);
    no_exception(env, "native_thread");
    CHECK(env, "native_thread", thread->result == 58);
    CHECK(NULL, "native_thread", (*thread->vm)->DetachCurrentThread(thread->vm) == JNI_OK);
    env = NULL;
    CHECK(NULL, "native_thread", (*thread->vm)->GetEnv(thread->vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED);
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    CHECK(NULL, "arguments", (argc == 2 || (argc == 3 && !strcmp(argv[2], "jit-first-use"))) && argv[1][0] == '/');
    bool first_use = argc == 3;
    printf("{\"stock_art_config\":true,\"jni_version\":%d,\"jvalue_bytes\":%zu,\"jit_requested\":true,\"jit_first_use\":%s}\n", JNI_VERSION_1_6, sizeof(jvalue), first_use ? "true" : "false");
    const char *environment[][2] = {
        {"ANDROID_ROOT", "/system"},
        {"ANDROID_DATA", "/data"},
        {"ANDROID_ART_ROOT", "/apex/com.android.art"},
        {"ANDROID_I18N_ROOT", "/apex/com.android.i18n"},
        {"ANDROID_TZDATA_ROOT", "/apex/com.android.tzdata"},
        {"ICU_DATA", "/apex/com.android.i18n/etc/icu"},
    };
    for (size_t i = 0; i < sizeof(environment) / sizeof(*environment); ++i)
        CHECK(NULL, "environment", setenv(environment[i][0], environment[i][1], 1) == 0);
    __android_log_set_logger(__android_log_stderr_logger);
    snapshot("before-vm");
    void *library = dlopen("libart.so", RTLD_NOW | RTLD_GLOBAL);
    if (!library)
        fprintf(stderr, "stock-art dlopen: %s\n", dlerror());
    CHECK(NULL, "library", library);
    jint (*create)(JavaVM **, JNIEnv **, void *) = dlsym(library, "JNI_CreateJavaVM");
    jint (*created)(JavaVM **, jsize, jsize *) = dlsym(library, "JNI_GetCreatedJavaVMs");
    CHECK(NULL, "library", create && created);
    jsize count = -1;
    CHECK(NULL, "vm_absent", created(NULL, 0, &count) == JNI_OK && count == 0);
    passed("vm_absent", count);
    char *boot = NULL;
    CHECK(NULL, "options", asprintf(&boot, "-Xbootclasspath:%s", argv[1]) > 0);
    JavaVMOption options[] = {
        {.optionString = "-Xcheck:jni"},
        {.optionString = "-Xusejit:true"},
        {.optionString = "-Xnoimage-dex2oat"},
        {.optionString = "-Ximage:/data/stock-art/no-boot.art"},
        {.optionString = "-Xms8m"},
        {.optionString = "-Xmx64m"},
        {.optionString = boot},
        {.optionString = "-Djava.class.path=/data/stock-art/StockArtProbe.dex"},
        {.optionString = "-Djava.library.path=/system/lib64"},
        {.optionString = "-verbose:gc"},
        {.optionString = "-Xjitthreshold:0"},
    };
    JavaVMInitArgs args = {.version = JNI_VERSION_1_6, .nOptions = sizeof(options) / sizeof(*options) - !first_use, .options = options, .ignoreUnrecognized = JNI_FALSE};
    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    puts("{\"stock_art_stage\":\"create_vm\"}");
    jint create_result = create(&vm, &env, &args);
    CHECK(NULL, "create_vm", create_result == JNI_OK && vm && env);
    free(boot);
    JavaVM *observed_vm = NULL;
    CHECK(env, "create_vm", created(&observed_vm, 1, &count) == JNI_OK && count == 1 && observed_vm == vm);
    passed("create_vm", create_result);
    snapshot("after-vm");

    jclass local_type = (*env)->FindClass(env, "StockArtProbe");
    CHECK(env, "load_dex", local_type);
    jclass type = (*env)->NewGlobalRef(env, local_type);
    CHECK(env, "load_dex", type);
    (*env)->DeleteLocalRef(env, local_type);
    passed("load_dex", 1);
    jstring token = (*env)->NewStringUTF(env, "jni-token");
    CHECK(env, "primitive_arguments", token);
    jvalue values[] = {{.z = JNI_TRUE}, {.b = -101}, {.c = 0x4e2d}, {.s = -12345}, {.i = 0x12345678}, {.j = TOKEN}, {.f = 1.25f}, {.d = -123.5}, {.l = token}};
    jmethodID primitives = method(env, type, "primitiveArguments", "(ZBCSIJFDLjava/lang/Object;)J");
    jlong primitive_result = (*env)->CallStaticLongMethodA(env, type, primitives, values);
    no_exception(env, "primitive_arguments");
    CHECK(env, "primitive_arguments", primitive_result == TOKEN);
    passed("primitive_arguments", primitive_result);

    JNINativeMethod natives[] = {
        {"nativeIdentity", "(Ljava/lang/Object;)Ljava/lang/Object;", native_identity},
        {"nativeValues", "(ZBCSIJFDLjava/lang/Object;)J", native_values},
        {"nativeThrow", "()V", native_throw},
    };
    CHECK(env, "registered_natives", (*env)->RegisterNatives(env, type, natives, sizeof(natives) / sizeof(*natives)) == JNI_OK);
    jmethodID registered = method(env, type, "registeredNatives", "(Ljava/lang/Object;)J");
    jvalue token_argument = {.l = token};
    jlong native_result = (*env)->CallStaticLongMethodA(env, type, registered, &token_argument);
    no_exception(env, "registered_natives");
    CHECK(env, "registered_natives", native_result == TOKEN);
    (*env)->DeleteLocalRef(env, token);
    passed("registered_natives", native_result);

    jvalue box_value = {.j = TOKEN};
    jobject box = (*env)->CallStaticObjectMethodA(env, type, method(env, type, "makeBox", "(J)LStockArtProbe$Box;"), &box_value);
    no_exception(env, "global_reference");
    CHECK(env, "global_reference", box);
    jobject strong = (*env)->NewGlobalRef(env, box);
    jweak weak = (*env)->NewWeakGlobalRef(env, box);
    CHECK(env, "global_reference", strong && weak && (*env)->GetObjectRefType(env, strong) == JNIGlobalRefType && (*env)->GetObjectRefType(env, weak) == JNIWeakGlobalRefType);
    (*env)->DeleteLocalRef(env, box);
    jint attempts = (*env)->CallStaticIntMethod(env, type, method(env, type, "collectUnreachable", "()I"));
    no_exception(env, "gc_observed");
    CHECK(env, "gc_observed", attempts >= 1 && attempts <= 32);
    passed("gc_observed", attempts);
    jobject promoted = (*env)->NewLocalRef(env, weak);
    CHECK(env, "global_reference", promoted && (*env)->IsSameObject(env, promoted, strong));
    jclass box_type = (*env)->GetObjectClass(env, strong);
    CHECK(env, "global_reference", box_type);
    jfieldID field = (*env)->GetFieldID(env, box_type, "value", "J");
    CHECK(env, "global_reference", field);
    jlong box_result = (*env)->GetLongField(env, strong, field);
    no_exception(env, "global_reference");
    CHECK(env, "global_reference", box_result == TOKEN);
    (*env)->DeleteLocalRef(env, box_type);
    (*env)->DeleteLocalRef(env, promoted);
    passed("global_reference", box_result);
    snapshot("after-gc");

    jintArray array = (*env)->NewIntArray(env, 3);
    CHECK(env, "array_storage", array);
    const jint initial[] = {17, 23, 31};
    (*env)->SetIntArrayRegion(env, array, 0, 3, initial);
    no_exception(env, "array_storage");
    jboolean is_copy = JNI_FALSE;
    jint *storage = (*env)->GetPrimitiveArrayCritical(env, array, &is_copy);
    CHECK(env, "array_storage", storage);
    uintptr_t storage_address = (uintptr_t)storage;
    int contents_ok = storage[0] == 17 && storage[1] == 23 && storage[2] == 31;
    storage[2] = 47;
    (*env)->ReleasePrimitiveArrayCritical(env, array, storage, 0);
    CHECK(env, "array_storage", contents_ok);
    jint actual[3];
    (*env)->GetIntArrayRegion(env, array, 0, 3, actual);
    no_exception(env, "array_storage");
    CHECK(env, "array_storage", actual[0] == 17 && actual[1] == 23 && actual[2] == 47);
    (*env)->DeleteLocalRef(env, array);
    printf("{\"stock_art_array_storage\":true,\"address\":%" PRIuPTR ",\"is_copy\":%s}\n", storage_address, is_copy ? "true" : "false");
    passed("array_storage", actual[2]);

    jint java_threads = (*env)->CallStaticIntMethod(env, type, method(env, type, "javaThreads", "()I"));
    no_exception(env, "java_threads");
    CHECK(env, "java_threads", java_threads == 2000);
    passed("java_threads", java_threads);
    struct AttachedThread attached = {.vm = vm, .type = type};
    pthread_t thread;
    CHECK(env, "native_thread", pthread_create(&thread, NULL, attached_thread, &attached) == 0);
    CHECK(env, "native_thread", pthread_join(thread, NULL) == 0 && attached.result == 58);
    passed("native_thread", attached.result);

    (*env)->CallStaticVoidMethod(env, type, method(env, type, "throwExpected", "()V"));
    CHECK(NULL, "java_exception", (*env)->ExceptionCheck(env));
    jthrowable exception = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    jclass expected_type = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
    CHECK(env, "java_exception", exception && expected_type && (*env)->IsInstanceOf(env, exception, expected_type));
    jmethodID get_message = (*env)->GetMethodID(env, expected_type, "getMessage", "()Ljava/lang/String;");
    CHECK(env, "java_exception", get_message);
    jstring message = (*env)->CallObjectMethod(env, exception, get_message);
    no_exception(env, "java_exception");
    CHECK(env, "java_exception", message);
    const char *text = (*env)->GetStringUTFChars(env, message, NULL);
    CHECK(env, "java_exception", text && strcmp(text, "java-expected") == 0);
    (*env)->ReleaseStringUTFChars(env, message, text);
    (*env)->DeleteLocalRef(env, message);
    (*env)->DeleteLocalRef(env, expected_type);
    (*env)->DeleteLocalRef(env, exception);
    passed("java_exception", 1);

    const jint iterations = 200000;
    CHECK(NULL, "observation", prctl(PR_SET_NAME, "art-jit-begin", 0, 0, 0) == 0);
    snapshot("before-warmup");
    jvalue warmup_argument = {.i = iterations};
    jlong warmed = (*env)->CallStaticLongMethodA(env, type, method(env, type, "warmup", "(I)J"), &warmup_argument);
    no_exception(env, "warmup_result");
    int64_t expected = 0;
    for (int i = 0; i < iterations; ++i)
        expected += ((i * 17) ^ (i >> 3)) + 9;
    CHECK(env, "warmup_result", warmed == expected);
    passed("warmup_result", warmed);
    printf("{\"stock_art_jit\":\"unverified\",\"warmup_calls\":%d}\n", iterations);
    snapshot("after-warmup");
    CHECK(NULL, "observation", prctl(PR_SET_NAME, "art-jit-end", 0, 0, 0) == 0);
    (*env)->DeleteWeakGlobalRef(env, weak);
    (*env)->DeleteGlobalRef(env, strong);
    CHECK(env, "unregister_natives", (*env)->UnregisterNatives(env, type) == JNI_OK);
    (*env)->DeleteGlobalRef(env, type);
    puts("{\"stock_art_stage\":\"destroy_vm\"}");
    jint destroyed = (*vm)->DestroyJavaVM(vm);
    CHECK(NULL, "destroy_vm", destroyed == JNI_OK);
    CHECK(NULL, "destroy_vm", created(NULL, 0, &count) == JNI_OK && count == 0);
    passed("destroy_vm", destroyed);
    puts("{\"test\":\"stock_art_standard_jni\",\"passed\":true,\"jit_execution_confirmed\":false}");
    return 0;
}
