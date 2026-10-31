#include <jni.h>
#include "flashctl.h"

JNIEXPORT jint JNICALL
Java_com_example_veuxtorch_FlashNative_nativeOn(JNIEnv *env, jclass clazz, jint level) {
    (void)env;
    (void)clazz;
    return (jint)fl_on((int)level);
}

JNIEXPORT jint JNICALL
Java_com_example_veuxtorch_FlashNative_nativeSetLevel(JNIEnv *env, jclass clazz, jint level) {
    (void)env;
    (void)clazz;
    return (jint)fl_set_level((int)level);
}

JNIEXPORT jint JNICALL
Java_com_example_veuxtorch_FlashNative_nativeOff(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return (jint)fl_off();
}

JNIEXPORT jint JNICALL
Java_com_example_veuxtorch_FlashNative_nativeHeartbeat(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return (jint)fl_heartbeat();
}

JNIEXPORT jintArray JNICALL
Java_com_example_veuxtorch_FlashNative_nativeStatus(JNIEnv *env, jclass clazz) {
    (void)clazz;
    struct fl_status_info info = fl_status();

    jintArray result = (*env)->NewIntArray(env, 4);
    if (!result) return NULL;

    jint fill[4];
    fill[0] = (jint)info.is_on;
    fill[1] = (jint)info.current_level;
    fill[2] = (jint)info.seconds_on;
    fill[3] = (jint)info.cooldown_remaining;

    (*env)->SetIntArrayRegion(env, result, 0, 4, fill);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_example_veuxtorch_FlashNative_nativeMaxLevel(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return (jint)fl_max_level();
}

JNIEXPORT jstring JNICALL
Java_com_example_veuxtorch_FlashNative_nativeStrerror(JNIEnv *env, jclass clazz, jint err) {
    (void)clazz;
    const char *str = fl_strerror((int)err);
    return (*env)->NewStringUTF(env, str ? str : "Unknown error");
}
