// SPDX-License-Identifier: GPL-3.0-or-later
// Controller rumble on Android: the game's HID vibration (hle_hid.cpp -> pad_rumble.h, the same strengths the desktop
// backends send through SDL / XInput) goes to the vibration motors of the controller that last sent input.
//
// Android 12+ (API 31) gives each motor of an input device through InputDevice.getVibratorManager(): two motors get
// the left and right strengths, one motor the stronger of the two. Older versions only have InputDevice.getVibrator(),
// a single motor. The phone's own vibrator is never used. Called from the app's main loop; JNI calls happen only when
// the strengths change or every 250 ms while they are non-zero, each one a 500 ms pulse, as on the desktop.
// NX_RUMBLE=0 turns it off.
#include <android/api-level.h>
#include <jni.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <nx/runtime.h>

#include "pad_rumble.h"
#include "platform.h"
#include "platform_android.h"

namespace nx::plat {

std::atomic<int32_t> g_pad_device{-1};

namespace {

struct Motors {
    int32_t device = -1;
    jobject manager = nullptr;  // VibratorManager (API 31+), global ref
    jobject vibrator = nullptr; // Vibrator (one motor), global ref
    jint ids[2] = {0, 0};
    int count = 0;              // motors usable: 0, 1 or 2
};

bool clear_exception(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    log("rumble: %s failed (Java exception)", what);
    return true;
}

void release(JNIEnv* env, Motors& m) {
    if (m.manager) env->DeleteGlobalRef(m.manager);
    if (m.vibrator) env->DeleteGlobalRef(m.vibrator);
    m = Motors{};
}

// The motors of input device `id`, or count 0.
Motors open_motors(JNIEnv* env, int32_t id) {
    Motors m;
    m.device = id;
    jclass input_device = env->FindClass("android/view/InputDevice");
    if (!input_device) return clear_exception(env, "InputDevice"), m;
    jobject dev = env->CallStaticObjectMethod(
        input_device, env->GetStaticMethodID(input_device, "getDevice", "(I)Landroid/view/InputDevice;"), id);
    if (clear_exception(env, "InputDevice.getDevice") || !dev) return m;
    jstring name = static_cast<jstring>(
        env->CallObjectMethod(dev, env->GetMethodID(input_device, "getName", "()Ljava/lang/String;")));
    const char* name_c = name && !env->ExceptionCheck() ? env->GetStringUTFChars(name, nullptr) : nullptr;
    clear_exception(env, "InputDevice.getName");
    if (android_get_device_api_level() >= 31) {
        jobject mgr = env->CallObjectMethod(
            dev, env->GetMethodID(input_device, "getVibratorManager", "()Landroid/os/VibratorManager;"));
        if (!clear_exception(env, "getVibratorManager") && mgr) {
            jclass vm = env->GetObjectClass(mgr);
            jintArray ids = static_cast<jintArray>(env->CallObjectMethod(mgr, env->GetMethodID(vm, "getVibratorIds", "()[I")));
            if (!clear_exception(env, "getVibratorIds") && ids) {
                const jsize n = env->GetArrayLength(ids);
                if (n > 0) {
                    env->GetIntArrayRegion(ids, 0, n >= 2 ? 2 : 1, m.ids);
                    m.count = n >= 2 ? 2 : 1;
                    m.manager = env->NewGlobalRef(mgr);
                }
                log("rumble: controller %d \"%s\": %d motor(s)", id, name_c ? name_c : "?", int(n));
            }
        }
    } else {
        jobject vib = env->CallObjectMethod(dev, env->GetMethodID(input_device, "getVibrator", "()Landroid/os/Vibrator;"));
        if (!clear_exception(env, "getVibrator") && vib) {
            jclass vc = env->GetObjectClass(vib);
            const bool has = env->CallBooleanMethod(vib, env->GetMethodID(vc, "hasVibrator", "()Z"));
            if (!clear_exception(env, "hasVibrator") && has) {
                m.vibrator = env->NewGlobalRef(vib);
                m.count = 1;
            }
            log("rumble: controller %d \"%s\": %s", id, name_c ? name_c : "?", m.count ? "one motor" : "no motor");
        }
    }
    if (name_c) env->ReleaseStringUTFChars(name, name_c);
    return m;
}

jobject one_shot(JNIEnv* env, uint16_t strength) {
    jclass ve = env->FindClass("android/os/VibrationEffect");
    const jint amplitude = jint(1 + (uint32_t(strength) * 254u) / 65535u);  // 1..255
    jobject e = env->CallStaticObjectMethod(ve, env->GetStaticMethodID(ve, "createOneShot", "(JI)Landroid/os/VibrationEffect;"),
                                            jlong(500), amplitude);
    return clear_exception(env, "VibrationEffect.createOneShot") ? nullptr : e;
}

void cancel(JNIEnv* env, const Motors& m) {
    if (m.manager) env->CallVoidMethod(m.manager, env->GetMethodID(env->GetObjectClass(m.manager), "cancel", "()V"));
    if (m.vibrator) env->CallVoidMethod(m.vibrator, env->GetMethodID(env->GetObjectClass(m.vibrator), "cancel", "()V"));
    clear_exception(env, "cancel");
}

void vibrate(JNIEnv* env, const Motors& m, uint16_t left, uint16_t right) {
    if (!left && !right) return cancel(env, m);
    if (m.vibrator) {
        if (jobject e = one_shot(env, left > right ? left : right)) {
            env->CallVoidMethod(m.vibrator, env->GetMethodID(env->GetObjectClass(m.vibrator), "vibrate",
                                                             "(Landroid/os/VibrationEffect;)V"), e);
            clear_exception(env, "Vibrator.vibrate");
            env->DeleteLocalRef(e);
        }
        return;
    }
    if (!m.manager) return;
    jclass cv = env->FindClass("android/os/CombinedVibration");
    jobject combo = env->CallStaticObjectMethod(
        cv, env->GetStaticMethodID(cv, "startParallel", "()Landroid/os/CombinedVibration$ParallelCombination;"));
    if (clear_exception(env, "CombinedVibration.startParallel") || !combo) return;
    jclass pc = env->GetObjectClass(combo);
    jmethodID add = env->GetMethodID(pc, "addVibrator", "(ILandroid/os/VibrationEffect;)Landroid/os/CombinedVibration$ParallelCombination;");
    const uint16_t strengths[2] = {m.count == 2 ? left : (left > right ? left : right), right};
    for (int k = 0; k < m.count; k++) {
        if (!strengths[k]) continue;
        jobject e = one_shot(env, strengths[k]);
        if (!e) return;
        env->CallObjectMethod(combo, add, m.ids[k], e);
        env->DeleteLocalRef(e);
        if (clear_exception(env, "addVibrator")) return;
    }
    jobject done = env->CallObjectMethod(combo, env->GetMethodID(pc, "combine", "()Landroid/os/CombinedVibration;"));
    if (clear_exception(env, "combine") || !done) return;
    env->CallVoidMethod(m.manager, env->GetMethodID(env->GetObjectClass(m.manager), "vibrate",
                                                    "(Landroid/os/CombinedVibration;)V"), done);
    clear_exception(env, "VibratorManager.vibrate");
}

}  // namespace

void android_rumble_tick(android_app* app) {
    static const bool off = [] {
        const char* e = std::getenv("NX_RUMBLE");
        return e && e[0] == '0';
    }();
    if (off) return;
    static JNIEnv* env = nullptr;
    if (!env && app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
    if (!env) return;
    static Motors motors;
    static uint32_t last = UINT32_MAX;
    static uint64_t sent_at = 0;
    const int32_t device = g_pad_device.load(std::memory_order_relaxed);
    if (device != motors.device) {  // another controller: the old one stops, the game's strengths start from zero
        if (motors.count) cancel(env, motors);
        release(env, motors);
        reset_pad_rumble();
        last = UINT32_MAX;
        env->PushLocalFrame(32);
        if (device >= 0) motors = open_motors(env, device);
        else motors.device = device;
        env->ExceptionClear();
        env->PopLocalFrame(nullptr);
    }
    if (!motors.count) return;
    const uint32_t rumble = read_pad_rumble();
    const uint64_t now = monotonic_ns() / 1'000'000;
    const bool changed = rumble != last;
    if ((changed && (rumble || last != UINT32_MAX)) || (rumble && now - sent_at >= 250)) {
        env->PushLocalFrame(16);
        vibrate(env, motors, uint16_t(rumble), uint16_t(rumble >> 16));
        env->PopLocalFrame(nullptr);
        last = rumble;
        sent_at = now;
    }
}

}  // namespace nx::plat
