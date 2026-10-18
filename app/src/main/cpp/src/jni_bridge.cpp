#include <jni.h>
#include <android/log.h>
#include <string>
#include <memory>
#include <atomic>
#include <utility>

#include "vban_engine.hpp"

#define LOG_TAG "VbanJniBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

// Cached global references for NativeTelemetry class and constructor
jclass    g_telemetryClass = nullptr;
jmethodID g_telemetryCtor  = nullptr;

/**
 * RAII helper for jstring conversion to ensure ReleaseStringUTFChars is always invoked.
 */
class JniUtfString {
public:
    JniUtfString(JNIEnv* env, jstring javaStr)
        : m_env(env), m_javaStr(javaStr), m_chars(nullptr) {
        if (m_env && m_javaStr) {
            m_chars = m_env->GetStringUTFChars(m_javaStr, nullptr);
        }
    }

    ~JniUtfString() {
        if (m_env && m_javaStr && m_chars) {
            m_env->ReleaseStringUTFChars(m_javaStr, m_chars);
        }
    }

    [[nodiscard]] const char* c_str() const noexcept {
        return m_chars ? m_chars : "";
    }

    [[nodiscard]] std::string toString() const {
        return m_chars ? std::string(m_chars) : std::string();
    }

    JniUtfString(const JniUtfString&) = delete;
    JniUtfString& operator=(const JniUtfString&) = delete;

private:
    JNIEnv*     m_env;
    jstring     m_javaStr;
    const char* m_chars;
};

} // anonymous namespace

// ==============================================================================
// JNI Lifecycle Hooks
// ==============================================================================

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    LOGI("JNI_OnLoad: Initializing VBAN Receptor JNI Bridge");
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("JNI_OnLoad: Failed to get JNIEnv from JavaVM");
        return JNI_ERR;
    }

    jclass localClass = env->FindClass("com/vban/receptor/native/NativeTelemetry");
    if (!localClass) {
        LOGE("JNI_OnLoad: Could not find com/vban/receptor/native/NativeTelemetry");
        return JNI_ERR;
    }

    g_telemetryClass = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
    env->DeleteLocalRef(localClass);
    if (!g_telemetryClass) {
        LOGE("JNI_OnLoad: Failed to create global reference for NativeTelemetry");
        return JNI_ERR;
    }

    // Constructor descriptor: (sampleRate: Int, channelCount: Int, bitDepth: Int,
    // totalPackets: Long, lostPackets: Long, jitterMs: Float, bufferOccupancy: Float,
    // peakL: Float, peakR: Float) -> Void
    g_telemetryCtor = env->GetMethodID(g_telemetryClass, "<init>", "(IIIJJFFFF)V");
    if (!g_telemetryCtor) {
        LOGE("JNI_OnLoad: Could not find NativeTelemetry constructor (IIIJJFFFF)V");
        env->DeleteGlobalRef(g_telemetryClass);
        g_telemetryClass = nullptr;
        return JNI_ERR;
    }

    LOGI("JNI_OnLoad: Successfully cached NativeTelemetry class & constructor");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* /*reserved*/) {
    LOGI("JNI_OnUnload: Releasing cached JNI references");
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        if (g_telemetryClass) {
            env->DeleteGlobalRef(g_telemetryClass);
            g_telemetryClass = nullptr;
            g_telemetryCtor  = nullptr;
        }
    }
}

// ==============================================================================
// Native Engine JNI Exports
// ==============================================================================

extern "C" {

/**
 * Initializes the C++20 VBAN native engine, verifying CPU capabilities,
 * concepts, and Oboe environment.
 */
JNIEXPORT jboolean JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_initEngine(
    JNIEnv* /*env*/, jobject /*thiz*/) {
    bool ok = vban::VbanEngine::getInstance().initialize();
    LOGI("initEngine() returned %s", ok ? "true" : "false");
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Starts the VBAN network receiver and Oboe playback pipeline.
 */
JNIEXPORT jboolean JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_startReceiver(
    JNIEnv* env,
    jobject /*thiz*/,
    jstring listenIp,
    jint port,
    jstring streamName,
    jint deviceId) {
    if (!env) {
        return JNI_FALSE;
    }

    JniUtfString ipStr(env, listenIp);
    JniUtfString nameStr(env, streamName);

    std::string ip = ipStr.toString();
    if (ip.empty()) {
        ip = "0.0.0.0";
    }

    std::string name = nameStr.toString();
    uint16_t uPort = (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : 6980;

    LOGI("startReceiver: IP=%s, Port=%u, Stream='%s', DeviceId=%d",
         ip.c_str(), uPort, name.c_str(), deviceId);

    bool result = vban::VbanEngine::getInstance().start(ip, uPort, name, deviceId);
    return result ? JNI_TRUE : JNI_FALSE;
}

/**
 * Stops the VBAN network receiver, tears down UDP socket, and stops Oboe player.
 */
JNIEXPORT void JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_stopReceiver(
    JNIEnv* /*env*/, jobject /*thiz*/) {
    LOGI("stopReceiver called from JNI");
    vban::VbanEngine::getInstance().stop();
}

/**
 * Routes audio playback to a specific Android audio device (e.g. USB DAC).
 */
JNIEXPORT void JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_setAudioDevice(
    JNIEnv* /*env*/, jobject /*thiz*/, jint deviceId) {
    LOGI("setAudioDevice called from JNI: deviceId=%d", deviceId);
    vban::VbanEngine::getInstance().setAudioDevice(deviceId);
}

/**
 * Non-blocking, lock-free telemetry retrieval. Returns an instance of NativeTelemetry.
 */
JNIEXPORT jobject JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_getTelemetry(
    JNIEnv* env, jobject /*thiz*/) {
    if (!env) {
        return nullptr;
    }

    // Retrieve lock-free atomic snapshot from native coordinator
    const auto telemetry = vban::VbanEngine::getInstance().getTelemetry();

    jclass cls = g_telemetryClass;
    jmethodID ctor = g_telemetryCtor;

    // Fallback lazy resolution if JNI_OnLoad was bypassed
    if (!cls || !ctor) {
        jclass localCls = env->FindClass("com/vban/receptor/native/NativeTelemetry");
        if (!localCls) {
            LOGW("getTelemetry: Unable to resolve NativeTelemetry class");
            return nullptr;
        }
        cls = localCls;
        ctor = env->GetMethodID(cls, "<init>", "(IIIJJFFFF)V");
        if (!ctor) {
            LOGW("getTelemetry: Unable to resolve NativeTelemetry constructor");
            return nullptr;
        }
    }

    return env->NewObject(
        cls,
        ctor,
        static_cast<jint>(telemetry.sampleRate),
        static_cast<jint>(telemetry.channelCount),
        static_cast<jint>(telemetry.bitDepth),
        static_cast<jlong>(telemetry.totalPacketsReceived),
        static_cast<jlong>(telemetry.lostPackets),
        static_cast<jfloat>(telemetry.jitterMs),
        static_cast<jfloat>(telemetry.bufferOccupancyPercent),
        static_cast<jfloat>(telemetry.peakLevelLeft),
        static_cast<jfloat>(telemetry.peakLevelRight)
    );
}

/**
 * Configures target audio latency / buffer watermark in milliseconds.
 */
JNIEXPORT jboolean JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_setTargetLatencyMs(
    JNIEnv* /*env*/, jobject /*thiz*/, jfloat targetMs) {
    bool ok = vban::VbanEngine::getInstance().setTargetLatencyMs(static_cast<float>(targetMs));
    return ok ? JNI_TRUE : JNI_FALSE;
}

/**
 * Polls latest real-time FFT spectrum bands for 60 FPS UI visualizer.
 */
JNIEXPORT jboolean JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_getSpectrumBands(
    JNIEnv* env, jobject /*thiz*/, jfloatArray outArray) {
    if (!env || !outArray) {
        return JNI_FALSE;
    }

    jsize len = env->GetArrayLength(outArray);
    if (len <= 0) {
        return JNI_FALSE;
    }

    jfloat* elems = env->GetFloatArrayElements(outArray, nullptr);
    if (!elems) {
        return JNI_FALSE;
    }

    bool hasNew = vban::VbanEngine::getInstance().getLatestFftBands(
        elems, static_cast<size_t>(len)
    );

    env->ReleaseFloatArrayElements(outArray, elems, 0);
    return hasNew ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
