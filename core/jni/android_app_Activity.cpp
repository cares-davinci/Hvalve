/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <poll.h>

#include <string>

#include <sys/syscall.h>
#include <sys/types.h>

#include "core_jni_helpers.h"

//#define __NR_hvalve 443

extern "C" void android_dlwarning(void*, void (*)(void*, const char*));

namespace android
{

static jstring getDlWarning_native(JNIEnv* env, jobject) {
    std::string msg;
    android_dlwarning(&msg, [](void* obj, const char* msg) {
        if (msg != nullptr) {
            *reinterpret_cast<std::string*>(obj) = msg;
        }
    });

    return msg.empty() ? nullptr : env->NewStringUTF(msg.c_str());
}

static void updateForegroundUid_native(JNIEnv* env, jobject clazz, int uid) {
    syscall(__NR_hvalve, 2, uid, 0, 0);
}

static void updateCreateUid_native(JNIEnv* env, jobject clazz, int uid) {
    syscall(__NR_hvalve, 3, uid, 0, 0);
}

static const JNINativeMethod g_methods[] = {
    { "getDlWarning",
        "()Ljava/lang/String;",
        reinterpret_cast<void*>(getDlWarning_native) },
    { "updateForegroundUid",
        "(I)V",
        reinterpret_cast<void*>(updateForegroundUid_native) },
    { "updateCreateUid",
        "(I)V",
        reinterpret_cast<void*>(updateCreateUid_native) },
};

static const char* const kActivityPathName = "android/app/Activity";

int register_android_app_Activity(JNIEnv* env) {
    return RegisterMethodsOrDie(env, kActivityPathName, g_methods, NELEM(g_methods));
}

} // namespace android
