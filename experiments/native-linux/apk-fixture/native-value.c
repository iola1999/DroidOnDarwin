#include <jni.h>

JNIEXPORT jint JNICALL Java_app_droidondarwin_fixture_NativeBridge_value(JNIEnv *env, jclass type)
{
    (void)env;
    (void)type;
    return FIXTURE_VALUE;
}
