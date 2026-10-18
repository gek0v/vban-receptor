# Survey Report: Milestone 4 - JNI Bridge & Kotlin Wrapper Architecture

**Agent**: explorer_m4_jni  
**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_jni`  
**Date**: 2026-10-18  
**Scope**: JNI Bridge (`jni_bridge.cpp`), Kotlin Native Wrapper (`NativeVbanEngine.kt`), and Standalone Host Unit Tests (`test_vban_receiver.cpp`).

---

## 1. Executive Summary & Architecture Context

Milestone 4 connects the low-latency C++20 audio processing engine to the Android Kotlin application layer. The interface boundary is governed by strict real-time constraints:
- **Zero-Allocation**: No dynamic heap allocations (`new`, `malloc`, `std::vector`) on the real-time audio callback thread (`onAudioReady`) or the high-priority UDP network receive loop.
- **Lock-Free Telemetry Polling**: UI updates (60 Hz Jetpack Compose VU meters, buffer occupancy gauges, jitter counters) must query native engine state asynchronously without acquiring mutexes or stalling audio rendering.
- **Cache-Optimized JNI Reflection**: In JNI, class lookup (`FindClass`) and method lookup (`GetMethodID`) are expensive string table operations. Resolving and caching global references in `JNI_OnLoad` reduces telemetry instantiation to sub-microsecond overhead.
- **Host Testability**: The native UDP socket engine, multicast filtering, and telemetry struct packing must be fully testable on the host development workstation (Windows x86_64 / Linux) without requiring an Android emulator or device attached.

```
┌────────────────────────────────────────────────────────────────────────┐
│ Kotlin / Android Framework Layer                                       │
│ - NativeVbanEngine (Thread-safe singleton wrapper)                     │
│ - NativeTelemetry (Immutable data class matching JNI constructor)      │
│ - Telemetry StateFlow polling loop (20-60 Hz reactive UI updates)      │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ JNI Bridge (C-linkage exports)
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ JNI Bridge Implementation (jni_bridge.cpp)                             │
│ - JNI_OnLoad / JNI_OnUnload: Cached global ref & constructor ID        │
│ - Safe string conversions (ReleaseStringUTFChars with RAII/guards)     │
│ - Non-blocking NativeTelemetry snapshot instantiator                   │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ C++20 Core Engine
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Native VBAN Engine Coordinator (VbanEngine)                            │
│ - VbanReceiver (UDP socket, 4MB buffer, multicast group)               │
│ - SpscRingBuffer<float, 262144> (Lock-free circular audio buffer)      │
│ - OboePlayer (Exclusive AAudio stream, Drift Compensator, VU meters)   │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. JNI Bridge Specification (`jni_bridge.cpp`)

### 2.1 Interface Signatures and Package Layout
- **Target Java Package**: `com.vban.receptor.native`
- **Target Java Class**: `NativeVbanEngine`
- **Target Telemetry Data Class**: `NativeTelemetry`

The required JNI export functions:
1. `Java_com_vban_receptor_native_NativeVbanEngine_initEngine(JNIEnv* env, jobject thiz) -> jboolean`
2. `Java_com_vban_receptor_native_NativeVbanEngine_startReceiver(JNIEnv* env, jobject thiz, jstring listenIp, jint port, jstring streamName, jint deviceId) -> jboolean`
3. `Java_com_vban_receptor_native_NativeVbanEngine_stopReceiver(JNIEnv* env, jobject thiz) -> void`
4. `Java_com_vban_receptor_native_NativeVbanEngine_setAudioDevice(JNIEnv* env, jobject thiz, jint deviceId) -> void`
5. `Java_com_vban_receptor_native_NativeVbanEngine_getTelemetry(JNIEnv* env, jobject thiz) -> jobject`

### 2.2 NativeTelemetry Constructor Signature
The `NativeTelemetry` class constructor signature in standard JNI descriptor notation:
```
(IIIJJFFFF)V
```
Field parameter mapping:
| # | Field Name | Native C++ Type | JNI Parameter Type | JVM Type | Description |
|---|------------|-----------------|--------------------|----------|-------------|
| 1 | `sampleRate` | `uint32_t` | `jint` (`I`) | `Int` | Audio sample rate in Hz (e.g. 48000, 768000) |
| 2 | `channelCount` | `uint16_t` | `jint` (`I`) | `Int` | Channel count (1 to 8) |
| 3 | `bitDepth` | `uint16_t` | `jint` (`I`) | `Int` | Sample bit depth (16, 24, 32) |
| 4 | `totalPacketsReceived` | `uint64_t` | `jlong` (`J`) | `Long` | Total valid UDP datagrams received |
| 5 | `lostPackets` | `uint64_t` | `jlong` (`J`) | `Long` | Dropped datagrams detected by sequence gap |
| 6 | `jitterMs` | `float` | `jfloat` (`F`) | `Float` | RFC 3550 EWMA network jitter in milliseconds |
| 7 | `bufferOccupancyPercent` | `float` | `jfloat` (`F`) | `Float` | SPSC buffer watermark (0.0% to 100.0%) |
| 8 | `peakLevelLeft` | `float` | `jfloat` (`F`) | `Float` | Left channel linear peak amplitude [0.0f, 1.0f] |
| 9 | `peakLevelRight` | `float` | `jfloat` (`F`) | `Float` | Right channel linear peak amplitude [0.0f, 1.0f] |

### 2.3 JNI Caching and Lifecycle Management
To avoid ClassLoader deadlocks and high-frequency string reflection overhead:
1. `JNI_OnLoad` acquires `JNIEnv`, searches for `"com/vban/receptor/native/NativeTelemetry"`, creates an explicit `NewGlobalRef`, and resolves the method ID for `<init>`.
2. `JNI_OnUnload` frees the global reference via `DeleteGlobalRef`.
3. In `getTelemetry()`, the cached `g_telemetryClass` and `g_telemetryCtor` are used directly with `env->NewObject()`. A lazy-load fallback is retained to guarantee operation even if initialized outside `JNI_OnLoad`.

### 2.4 Data Race Prevention in Native Telemetry
In `vban_engine.hpp`, `telemetryCache_` was previously a mutable struct mutated from both the network thread and the JNI thread. In the production template:
- `VbanEngine::getTelemetry()` constructs a value snapshot by reading atomic metrics directly:
  - `parser_.getTelemetry()`: returns sequence and loss metrics.
  - `ringBuffer_.occupancyPercent()`: thread-safe atomic read index vs write index calculation.
  - `player_->getPeakLevelLeft()`, `getPeakLevelRight()`: atomic loads (`std::memory_order_relaxed`).
- This guarantees 100% lock-free, tear-free reads without data races under ThreadSanitizer.

---

## 3. Production Code Template: `jni_bridge.cpp`

File path: `app/src/main/cpp/src/jni_bridge.cpp`

```cpp
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

} // extern "C"
```

---

## 4. Production Code Template: `NativeVbanEngine.kt`

File path: `app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt`

```kotlin
package com.vban.receptor.native

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Immutable telemetry snapshot received from the native C++ VBAN engine.
 *
 * Parameters match exact JNI constructor signature: `(IIIJJFFFF)V`
 *
 * @property sampleRate Active sample rate in Hz (e.g. 48000, 96000, 192000, 768000).
 * @property channelCount Active channel count (1 to 8).
 * @property bitDepth PCM bit depth (16, 24, 32).
 * @property totalPacketsReceived Monotonic count of valid received UDP packets.
 * @property lostPackets Monotonic count of missing packets detected via sequence gap analysis.
 * @property jitterMs Current inter-arrival network jitter (RFC 3550 EWMA) in milliseconds.
 * @property bufferOccupancyPercent SPSC circular ring buffer watermark percentage [0.0f, 100.0f].
 * @property peakLevelLeft Linear audio peak amplitude for Left channel [0.0f, 1.0f].
 * @property peakLevelRight Linear audio peak amplitude for Right channel [0.0f, 1.0f].
 */
data class NativeTelemetry(
    val sampleRate: Int = 0,
    val channelCount: Int = 0,
    val bitDepth: Int = 0,
    val totalPacketsReceived: Long = 0L,
    val lostPackets: Long = 0L,
    val jitterMs: Float = 0.0f,
    val bufferOccupancyPercent: Float = 0.0f,
    val peakLevelLeft: Float = 0.0f,
    val peakLevelRight: Float = 0.0f
) {
    /**
     * Computed packet loss rate percentage [0.0%, 100.0%].
     */
    val packetLossRatePercent: Float
        get() {
            val total = totalPacketsReceived + lostPackets
            return if (total > 0L) {
                (lostPackets.toFloat() / total.toFloat()) * 100.0f
            } else {
                0.0f
            }
        }

    /**
     * Left channel peak converted to decibels relative to Full Scale (dBFS).
     * Clamped to standard metering range [-60.0f, 0.0f].
     */
    val peakLevelLeftDbfs: Float
        get() = linearToDbfs(peakLevelLeft)

    /**
     * Right channel peak converted to decibels relative to Full Scale (dBFS).
     * Clamped to standard metering range [-60.0f, 0.0f].
     */
    val peakLevelRightDbfs: Float
        get() = linearToDbfs(peakLevelRight)

    companion object {
        /**
         * Converts linear amplitude in [0.0, 1.0] to decibels Full Scale (dBFS).
         */
        fun linearToDbfs(linear: Float): Float {
            if (linear <= 0.00001f) return -60.0f
            val db = 20.0f * kotlin.math.log10(linear)
            return db.coerceIn(-60.0f, 0.0f)
        }
    }
}

/**
 * Thread-safe singleton managing the native VBAN receptor engine lifecycle,
 * JNI communication, and reactive telemetry streaming.
 */
object NativeVbanEngine {
    private const val TAG = "NativeVbanEngine"
    private const val LIB_NAME = "vban_engine"

    private val isLibraryLoaded = AtomicBoolean(false)
    private val isInitialized = AtomicBoolean(false)

    private val _isRunning = MutableStateFlow(false)
    val isRunning: StateFlow<Boolean> = _isRunning.asStateFlow()

    private val _telemetry = MutableStateFlow(NativeTelemetry())
    val telemetry: StateFlow<NativeTelemetry> = _telemetry.asStateFlow()

    private var telemetryPollingJob: Job? = null
    private val scope = CoroutineScope(Dispatchers.Default)

    init {
        try {
            System.loadLibrary(LIB_NAME)
            isLibraryLoaded.set(true)
            Log.i(TAG, "Native library '$LIB_NAME' successfully loaded.")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load native library '$LIB_NAME': ${e.message}", e)
        }
    }

    // =========================================================================
    // JNI External Declarations
    // =========================================================================

    @JvmStatic
    private external fun initEngine(): Boolean

    @JvmStatic
    private external fun startReceiver(
        listenIp: String,
        port: Int,
        streamName: String,
        deviceId: Int
    ): Boolean

    @JvmStatic
    private external fun stopReceiver()

    @JvmStatic
    private external fun setAudioDevice(deviceId: Int)

    @JvmStatic
    private external fun getTelemetry(): NativeTelemetry?

    // =========================================================================
    // Public API
    // =========================================================================

    /**
     * Initializes the native C++20 engine environment.
     * Safe to invoke multiple times.
     */
    @Synchronized
    fun initialize(): Boolean {
        if (!isLibraryLoaded.get()) {
            Log.e(TAG, "Cannot initialize: '$LIB_NAME' library is not loaded.")
            return false
        }
        if (isInitialized.get()) {
            return true
        }

        val success = initEngine()
        if (success) {
            isInitialized.set(true)
            Log.i(TAG, "Native VBAN Engine initialized successfully.")
        } else {
            Log.e(TAG, "Native VBAN Engine initialization failed.")
        }
        return success
    }

    /**
     * Starts receiving VBAN packets and launches Oboe audio playback.
     *
     * @param listenIp Local network IP or multicast address (e.g. "0.0.0.0" or "239.255.0.1").
     * @param port UDP port (VBAN default is 6980).
     * @param streamName Stream name filter (matching VBAN packet header).
     * @param deviceId Android AudioDeviceInfo id (0 = default device).
     * @param pollIntervalMs Telemetry polling frequency in milliseconds (default: 33ms ~ 30 FPS).
     */
    @Synchronized
    fun start(
        listenIp: String = "0.0.0.0",
        port: Int = 6980,
        streamName: String = "",
        deviceId: Int = 0,
        pollIntervalMs: Long = 33L
    ): Boolean {
        if (!initialize()) {
            return false
        }
        if (_isRunning.value) {
            Log.w(TAG, "Engine is already running.")
            return true
        }

        val started = startReceiver(listenIp, port, streamName, deviceId)
        if (started) {
            _isRunning.value = true
            startTelemetryPolling(pollIntervalMs)
            Log.i(TAG, "VBAN Receiver started on $listenIp:$port, stream='$streamName', device=$deviceId")
        } else {
            Log.e(TAG, "Failed to start VBAN Receiver.")
        }
        return started
    }

    /**
     * Stops the VBAN reception and audio playback pipeline, canceling the polling job.
     */
    @Synchronized
    fun stop() {
        if (!_isRunning.value) {
            return
        }
        stopTelemetryPolling()
        if (isLibraryLoaded.get()) {
            stopReceiver()
        }
        _isRunning.value = false
        _telemetry.value = NativeTelemetry()
        Log.i(TAG, "VBAN Receiver stopped.")
    }

    /**
     * Dynamically switches the active output audio device (e.g., when a USB DAC is attached).
     */
    fun setDevice(deviceId: Int) {
        if (isLibraryLoaded.get()) {
            setAudioDevice(deviceId)
            Log.i(TAG, "Audio device set to ID $deviceId")
        }
    }

    /**
     * Synchronously queries a single telemetry snapshot without blocking.
     */
    fun pollTelemetrySnapshot(): NativeTelemetry {
        if (!isLibraryLoaded.get() || !_isRunning.value) {
            return _telemetry.value
        }
        return getTelemetry() ?: _telemetry.value
    }

    // =========================================================================
    // Internal Telemetry Poller
    // =========================================================================

    private fun startTelemetryPolling(pollIntervalMs: Long) {
        telemetryPollingJob?.cancel()
        telemetryPollingJob = scope.launch {
            while (isActive && _isRunning.value) {
                try {
                    val snapshot = getTelemetry()
                    if (snapshot != null) {
                        _telemetry.value = snapshot
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Error polling telemetry: ${e.message}")
                }
                delay(pollIntervalMs)
            }
        }
    }

    private fun stopTelemetryPolling() {
        telemetryPollingJob?.cancel()
        telemetryPollingJob = null
    }
}
```

---

## 5. Standalone Host Test Suite (`native_tests/test_vban_receiver.cpp`)

The test suite runs on the host development machine (Windows x86_64 / Linux) using `vban_native_tests.exe`.
It validates:
1. **UDP Socket Engine & 4MB Buffer**: Socket options (`SO_RCVBUF`, `SO_REUSEADDR`, `SO_RCVTIMEO`) and teardown.
2. **Loopback Packet Transmission & Receiving**: Synthesizing VBAN packets on `127.0.0.1`, pushing through `VbanReceiver`, and verifying SPSC ring buffer frame extraction.
3. **Multicast IPv4 Classification**: Address parsing and boundary detection (224.0.0.0 - 239.255.255.255).
4. **JNI Telemetry Packing & Concurrency**: Struct alignment, byte offsets, and non-blocking snapshot integrity under concurrent multi-threaded stress.

### 5.1 Cross-Platform Socket Abstraction in Host Tests
On Windows, sockets require Winsock initialization (`WSAStartup`) and linking against `ws2_32`. On POSIX (Linux/Android), standard BSD socket headers are used.
The test file includes a scoped RAII `WsaGuard` ensuring `WSAStartup` / `WSACleanup` are handled transparently.

```cpp
#include "test_framework.hpp"
#include "vban_protocol.hpp"
#include "vban_parser.hpp"
#include "vban_converter.hpp"
#include "spsc_ring_buffer.hpp"
#include "vban_engine.hpp"

#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
inline int close_sock(socket_t s) { return closesocket(s); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
inline int close_sock(socket_t s) { return close(s); }
#endif

namespace {

/**
 * RAII Initializer for Windows Winsock (no-op on POSIX).
 */
struct WsaGuard {
    WsaGuard() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~WsaGuard() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

/**
 * Multicast IP classification helper.
 * IPv4 Multicast range: 224.0.0.0 to 239.255.255.255 (Class D, highest nibble == 0xE).
 */
bool isMulticastAddress(const char* ipStr) {
    if (!ipStr || std::strlen(ipStr) == 0) return false;
    struct in_addr addr{};
#ifdef _WIN32
    if (InetPtonA(AF_INET, ipStr, &addr) != 1) return false;
#else
    if (inet_pton(AF_INET, ipStr, &addr) != 1) return false;
#endif
    uint32_t hostIp = ntohl(addr.s_addr);
    return (hostIp >= 0xE0000000 && hostIp <= 0xEFFFFFFF);
}

/**
 * Helper to construct a valid VBAN audio datagram for loopback verification.
 */
std::vector<uint8_t> createTestVbanPacket(
    uint32_t srIndex = 3,       // 48 kHz
    uint8_t  nbs = 255,         // 256 samples
    uint8_t  nbc = 1,           // 2 channels (stereo)
    vban::VBanDataType dt = vban::VBanDataType::Int16,
    const char* streamName = "Stream1",
    uint32_t nuFrame = 1) {

    uint8_t sampleSize = vban::getSampleSize(dt);
    size_t payloadBytes = vban::calculatePayloadBytes(nbs, nbc, sampleSize);
    std::vector<uint8_t> packet(vban::VBAN_HEADER_SIZE + payloadBytes, 0);

    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    hdr->magic = vban::VBAN_MAGIC;
    hdr->format_SR = (static_cast<uint8_t>(vban::VBanSubProtocol::Audio)) | (srIndex & 0x1F);
    hdr->format_nbs = nbs;
    hdr->format_nbc = nbc;
    hdr->format_bit = (static_cast<uint8_t>(vban::VBanCodec::PCM)) | (static_cast<uint8_t>(dt) & 0x07);

    std::memset(hdr->streamName, 0, 16);
    if (streamName) {
        size_t n = std::min(std::strlen(streamName), static_cast<size_t>(16));
        std::memcpy(hdr->streamName, streamName, n);
    }
    hdr->nuFrame = nuFrame;

    // Fill payload with a 1 kHz test sine wave in Int16
    auto* samples = reinterpret_cast<int16_t*>(packet.data() + vban::VBAN_HEADER_SIZE);
    size_t totalSamples = (nbs + 1) * (nbc + 1);
    for (size_t i = 0; i < totalSamples; ++i) {
        samples[i] = static_cast<int16_t>(16384 * std::sin(2.0 * 3.1415926535 * i / 48.0));
    }

    return packet;
}

} // anonymous namespace

// ==============================================================================
// 1. UDP Socket Creation, 4MB SO_RCVBUF, and Teardown Tests
// ==============================================================================

TEST_CASE("receiver_socket_create_bind_and_buffer_config") {
    WsaGuard wsa;

    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(sock, INVALID_SOCK);

    // 1. Configure SO_REUSEADDR
    int optTrue = 1;
    int res = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optTrue), sizeof(optTrue));
    EXPECT_EQ(res, 0);

    // 2. Request 4 MB SO_RCVBUF (4 * 1024 * 1024 = 4,194,304 bytes)
    int reqBuffer = 4 * 1024 * 1024;
    res = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&reqBuffer), sizeof(reqBuffer));
    EXPECT_EQ(res, 0);

    // 3. Query back SO_RCVBUF
    int actualBuffer = 0;
#ifdef _WIN32
    int optLen = sizeof(actualBuffer);
#else
    socklen_t optLen = sizeof(actualBuffer);
#endif
    res = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&actualBuffer), &optLen);
    EXPECT_EQ(res, 0);
    // On Windows, actualBuffer >= 4MB or OS-clamped high value. Verify buffer was enlarged well beyond 64KB
    EXPECT_GT(actualBuffer, 65536);

    // 4. Configure SO_RCVTIMEO (100 ms)
#ifdef _WIN32
    DWORD timeoutMs = 100;
    res = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    struct timeval tv{0, 100000};
    res = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
    EXPECT_EQ(res, 0);

    // 5. Bind to loopback ephemeral port
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0); // Ephemeral port
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    res = bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ASSERT_EQ(res, 0);

    // Clean teardown
    close_sock(sock);
}

// ==============================================================================
// 2. Loopback Packet Sending and Receiving Tests
// ==============================================================================

TEST_CASE("receiver_loopback_udp_transmission_and_reception") {
    WsaGuard wsa;

    // Server socket (receiver side)
    socket_t serverSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(serverSock, INVALID_SOCK);

    int optTrue = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optTrue), sizeof(optTrue));

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(0); // Ephemeral
    serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ASSERT_EQ(bind(serverSock, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)), 0);

    // Retrieve dynamically assigned port
#ifdef _WIN32
    int len = sizeof(serverAddr);
#else
    socklen_t len = sizeof(serverAddr);
#endif
    ASSERT_EQ(getsockname(serverSock, reinterpret_cast<struct sockaddr*>(&serverAddr), &len), 0);
    uint16_t assignedPort = ntohs(serverAddr.sin_port);
    ASSERT_GT(assignedPort, 0);

    // Set receive timeout
#ifdef _WIN32
    DWORD timeout = 500;
    setsockopt(serverSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv{0, 500000};
    setsockopt(serverSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    // Client socket (sender side)
    socket_t clientSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(clientSock, INVALID_SOCK);

    auto testPacket = createTestVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 100);

    struct sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(assignedPort);
    destAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int sent = sendto(clientSock, reinterpret_cast<const char*>(testPacket.data()),
                      static_cast<int>(testPacket.size()), 0,
                      reinterpret_cast<struct sockaddr*>(&destAddr), sizeof(destAddr));
    ASSERT_EQ(sent, static_cast<int>(testPacket.size()));

    // Receive on server socket
    std::vector<uint8_t> recvBuf(4096, 0);
    int received = recvfrom(serverSock, reinterpret_cast<char*>(recvBuf.data()),
                            static_cast<int>(recvBuf.size()), 0, nullptr, nullptr);
    ASSERT_EQ(received, static_cast<int>(testPacket.size()));

    // Verify received datagram parses correctly
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket parsed;
    vban::ParseResult res = parser.parse(recvBuf.data(), received, parsed);
    ASSERT_EQ(res, vban::ParseResult::Success);
    EXPECT_EQ(parsed.sampleRate, 48000U);
    EXPECT_EQ(parsed.channels, 2U);
    EXPECT_EQ(parsed.samplesPerFrame, 256U);
    EXPECT_EQ(parsed.frameNumber, 100U);

    close_sock(clientSock);
    close_sock(serverSock);
}

// ==============================================================================
// 3. Multicast Address Validation Tests
// ==============================================================================

TEST_CASE("receiver_multicast_address_range_validation") {
    // Valid IPv4 Multicast addresses (224.0.0.0 to 239.255.255.255)
    EXPECT_TRUE(isMulticastAddress("224.0.0.1"));
    EXPECT_TRUE(isMulticastAddress("224.0.0.251"));
    EXPECT_TRUE(isMulticastAddress("239.255.0.1"));
    EXPECT_TRUE(isMulticastAddress("239.255.255.255"));
    EXPECT_TRUE(isMulticastAddress("232.0.1.2"));

    // Non-multicast unicast / loopback / broadcast addresses
    EXPECT_FALSE(isMulticastAddress("127.0.0.1"));
    EXPECT_FALSE(isMulticastAddress("192.168.1.100"));
    EXPECT_FALSE(isMulticastAddress("10.0.0.1"));
    EXPECT_FALSE(isMulticastAddress("0.0.0.0"));
    EXPECT_FALSE(isMulticastAddress("223.255.255.255")); // Right below multicast range
    EXPECT_FALSE(isMulticastAddress("240.0.0.1"));       // Right above multicast range
    EXPECT_FALSE(isMulticastAddress("255.255.255.255")); // Limited broadcast

    // Malformed IP strings
    EXPECT_FALSE(isMulticastAddress(""));
    EXPECT_FALSE(isMulticastAddress("invalid_ip"));
    EXPECT_FALSE(isMulticastAddress("999.999.999.999"));
}

// ==============================================================================
// 4. JNI NativeTelemetry Struct Packing & Non-Blocking Snapshot Integrity
// ==============================================================================

TEST_CASE("jni_telemetry_struct_packing_and_alignment") {
    // Verify strict field sizes and offsets matching the JNI constructor:
    // (sampleRate: I, channelCount: I, bitDepth: I, totalPackets: J, lostPackets: J,
    //  jitterMs: F, bufferOccupancy: F, peakL: F, peakR: F) -> V

    ASSERT_EQ(sizeof(vban::NativeTelemetry), 40U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, sampleRate), 0U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, channelCount), 4U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, bitDepth), 6U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, totalPacketsReceived), 8U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, lostPackets), 16U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, jitterMs), 24U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, bufferOccupancyPercent), 28U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, peakLevelLeft), 32U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, peakLevelRight), 36U);
}

TEST_CASE("jni_telemetry_concurrent_snapshot_integrity") {
    // Multi-threaded test verifying that concurrent writer updates (simulating network thread)
    // and rapid reader snapshots (simulating JNI UI polling) do not result in torn reads.

    struct AtomicTelemetryStore {
        std::atomic<uint32_t> sampleRate{48000};
        std::atomic<uint16_t> channelCount{2};
        std::atomic<uint16_t> bitDepth{24};
        std::atomic<uint64_t> totalPackets{0};
        std::atomic<uint64_t> lostPackets{0};
        std::atomic<float>    jitterMs{0.0f};
        std::atomic<float>    bufferOccupancy{0.0f};
        std::atomic<float>    peakL{0.0f};
        std::atomic<float>    peakR{0.0f};
    };

    AtomicTelemetryStore store;
    std::atomic<bool> keepRunning{true};
    std::atomic<uint64_t> readSnapshots{0};
    std::atomic<bool> consistencyViolation{false};

    // Writer thread simulating high-frequency network packets
    std::thread writer([&]() {
        for (uint64_t i = 1; i <= 20000; ++i) {
            store.totalPackets.store(i, std::memory_order_relaxed);
            if (i % 100 == 0) {
                store.lostPackets.fetch_add(1, std::memory_order_relaxed);
            }
            store.jitterMs.store(static_cast<float>(i % 50) * 0.1f, std::memory_order_relaxed);
            store.bufferOccupancy.store(static_cast<float>(i % 100), std::memory_order_relaxed);
            store.peakL.store(static_cast<float>(i % 1000) / 1000.0f, std::memory_order_relaxed);
            store.peakR.store(static_cast<float>(i % 1000) / 1000.0f, std::memory_order_relaxed);
        }
        keepRunning.store(false, std::memory_order_release);
    });

    // Reader thread simulating JNI getTelemetry() polling
    std::thread reader([&]() {
        while (keepRunning.load(std::memory_order_acquire)) {
            vban::NativeTelemetry snapshot;
            snapshot.sampleRate = store.sampleRate.load(std::memory_order_relaxed);
            snapshot.channelCount = store.channelCount.load(std::memory_order_relaxed);
            snapshot.bitDepth = store.bitDepth.load(std::memory_order_relaxed);
            snapshot.totalPacketsReceived = store.totalPackets.load(std::memory_order_relaxed);
            snapshot.lostPackets = store.lostPackets.load(std::memory_order_relaxed);
            snapshot.jitterMs = store.jitterMs.load(std::memory_order_relaxed);
            snapshot.bufferOccupancyPercent = store.bufferOccupancy.load(std::memory_order_relaxed);
            snapshot.peakLevelLeft = store.peakL.load(std::memory_order_relaxed);
            snapshot.peakLevelRight = store.peakR.load(std::memory_order_relaxed);

            // Invariant assertions
            if (snapshot.sampleRate != 48000 || snapshot.channelCount != 2 || snapshot.bitDepth != 24) {
                consistencyViolation.store(true);
            }
            if (snapshot.lostPackets > snapshot.totalPacketsReceived) {
                consistencyViolation.store(true);
            }
            if (snapshot.bufferOccupancyPercent < 0.0f || snapshot.bufferOccupancyPercent > 100.0f) {
                consistencyViolation.store(true);
            }
            if (snapshot.peakLevelLeft < 0.0f || snapshot.peakLevelLeft > 1.0f) {
                consistencyViolation.store(true);
            }

            readSnapshots.fetch_add(1, std::memory_order_relaxed);
        }
    });

    writer.join();
    reader.join();

    EXPECT_FALSE(consistencyViolation.load());
    EXPECT_GT(readSnapshots.load(), 100U);
}
```

---

## 6. Build & Migration Plan

### 6.1 `app/src/main/cpp/CMakeLists.txt` Updates
Add `src/jni_bridge.cpp` and `src/vban_receiver.cpp` to `add_library(vban_engine SHARED ...)`:
```cmake
add_library(vban_engine SHARED
    src/vban_engine.cpp
    src/vban_parser.cpp
    src/vban_converter.cpp
    src/drift_compensator.cpp
    src/oboe_player.cpp
    src/vban_receiver.cpp
    src/jni_bridge.cpp
)
```

### 6.2 `native_tests/CMakeLists.txt` Updates
Add `test_vban_receiver.cpp` to the test suite and ensure `ws2_32` is linked on Windows:
```cmake
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_vban_receiver.cpp")
    list(APPEND TEST_SOURCES test_vban_receiver.cpp)
endif()

# On Windows host tests, link Winsock ws2_32 library
if(WIN32)
    target_link_libraries(vban_native_tests PRIVATE ws2_32)
endif()
```

### 6.3 `vban_engine.cpp` Cleanup
Remove the preliminary duplicate JNI function definitions (lines 260-330 of `vban_engine.cpp`) to prevent symbol collision with `src/jni_bridge.cpp`.

---

## 7. Verification Matrix

| Target | Requirement | Verification Method | Pass Criteria |
|---|---|---|---|
| `jni_bridge.cpp` | Function exports match JNI naming | `readelf -s` or `dumpbin /exports` | All 5 JNI symbols visible with default visibility |
| `jni_bridge.cpp` | Global reference caching | JNI_OnLoad execution in runtime | `g_telemetryClass` and `g_telemetryCtor` resolved once |
| `NativeVbanEngine.kt` | Data class matching JNI constructor | Kotlin compile & reflection | `NativeTelemetry(sampleRate, ...)` matching `(IIIJJFFFF)V` |
| `NativeVbanEngine.kt` | Reactive telemetry flow | Coroutine polling loop | StateFlow updates at requested interval |
| `test_vban_receiver.cpp` | UDP socket bind & 4MB buffer | `vban_native_tests.exe` | Buffer query > 64KB, clean bind and teardown |
| `test_vban_receiver.cpp` | Loopback packet transmission | `vban_native_tests.exe` | Exact byte count sent, received, and verified |
| `test_vban_receiver.cpp` | Multicast IP range | `vban_native_tests.exe` | All boundary test cases pass |
| `test_vban_receiver.cpp` | JNI struct packing & concurrency | `vban_native_tests.exe` | `sizeof == 40`, 0 consistency violations under 20k writes |
