/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <sys/syscall.h>
#include <sys/types.h>

//#define __NR_hvalve 443

namespace android {

static void startLaunch_native(JNIEnv* env, jobject clazz, jstring str, int uid) {
    const char *cString = env->GetStringUTFChars(str, nullptr);

    syscall(__NR_bpf, 9878, cString, uid);
    env->ReleaseStringUTFChars(str, cString);
}

static void getLauncher_native(JNIEnv* env, jobject clazz, int launcher_uid) {
    syscall(__NR_hvalve, 1, launcher_uid, 0, 0);
}

/*
 * JNI registration.
 */
static const JNINativeMethod gMethods[] = {
        /* name, signature, funcPtr */
    { "startLaunch", "(Ljava/lang/String;I)V",
        reinterpret_cast<void*>(startLaunch_native) },
    { "getLauncher", "(I)V",
        reinterpret_cast<void*>(getLauncher_native) },
};

int register_android_server_wm_ActivityStarter(JNIEnv* env)
{
    return jniRegisterNativeMethods(env, "com/android/server/wm/ActivityStarter",
            gMethods, NELEM(gMethods));
}

}; // namespace android
