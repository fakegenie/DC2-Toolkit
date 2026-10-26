#include "android_files.h"
#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>

const char *port_android_documents(void) {
    static char path[1024];
    if (path[0]) return path;
    const char *external = SDL_GetAndroidExternalStoragePath();
    snprintf(path, sizeof path, "%s/", external ? external : SDL_GetAndroidInternalStoragePath());
    return path;
}

void port_android_saves(void) {
    char saves[1100];
    snprintf(saves, sizeof saves, "%ssaves", port_android_documents());
    SDL_CreateDirectory(saves);
    setenv("DC2_SAVE_ROOT", saves, 1);
}

void port_android_name(const char *path, char *name, size_t size) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jobject activity = SDL_GetAndroidActivity();
    jclass kind = (*env)->GetObjectClass(env, activity);
    jmethodID method = (*env)->GetMethodID(env, kind, "displayName", "(Ljava/lang/String;)Ljava/lang/String;");
    jstring uri = (*env)->NewStringUTF(env, path);
    jstring shown = (*env)->CallObjectMethod(env, activity, method, uri);
    const char *text = shown ? (*env)->GetStringUTFChars(env, shown, NULL) : NULL;
    snprintf(name, size, "%s", text ? text : "");
    if (text) (*env)->ReleaseStringUTFChars(env, shown, text);
    (*env)->DeleteLocalRef(env, shown);
    (*env)->DeleteLocalRef(env, uri);
    (*env)->DeleteLocalRef(env, kind);
    (*env)->DeleteLocalRef(env, activity);
}
