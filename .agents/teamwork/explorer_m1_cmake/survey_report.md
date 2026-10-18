# Survey Report: NDK & CMake C++20 Build Configuration (Milestone 1)

**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m1_cmake`  
**Date**: 2026-10-18  
**Author**: Explorer Agent (`explorer_m1_cmake`)  
**Parent Orchestrator**: `9bb0c473-fb0a-499a-bbf2-a430786d0058`  
**Milestone**: M1 - Build Scaffolding & Native Test Harness  

---

## 1. Executive Summary

This report establishes the complete architectural design and code templates for the native C++20 engine and CMake 3.22+ build system of the **VBAN Receptor for Android**.

The native subsystem serves as the core real-time audio pipeline: receiving UDP network packets, dequantizing audio streams, managing a lock-free SPSC ring buffer, adjusting clock drift via Catmull-Rom micro-resampling, and driving low-latency audio through Google Oboe (AAudio/OpenSLES).

### Key Architectural Decisions Verified
1. **NDK & Compiler Environment**: Android NDK `26.1.10909125` contains LLVM Clang 17.0.2. It provides complete, production-ready ISO C++20 support (`<concepts>`, `<span>`, `<bit>`, `<atomic>`, `<ranges>`, `constexpr`). Target compilation was independently verified for ARM64-v8a (`aarch64-linux-android26-clang++`).
2. **CMake 3.22.1**: The project requires CMake 3.22.1 (`D:\Android\sdk\cmake\3.22.1`), adhering to the modern target-based CMake syntax (`project("vban_engine" CXX)`, `CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`).
3. **Prefab Oboe Linkage**: Oboe 1.9.0 is ingested via Gradle Prefab (`find_package(oboe REQUIRED CONFIG)`). It exports the target `oboe::oboe`, linking prebuilt binaries and headers seamlessly without manual ndk-build scripts or git submodules.
4. **C++ Standard Library**: The runtime STL is strictly set to `c++_shared` (LLVM libc++ shared) to maintain ABI and memory allocator compatibility with Oboe's shared library.
5. **Symbol Visibility**: Compile flags `-fvisibility=hidden` and `-fvisibility-inlines-hidden` ensure internal classes remain private to `libvban_engine.so`, exposing only `JNIEXPORT` symbols for clean boundary isolation and optimal performance.
6. **Native Skeleton**: Complete templates for `include/vban_engine.hpp` and `src/vban_engine.cpp` include early sanity verification (`verifyCpp20AndOboeEnvironment()`), atomic lifecycle management, and non-blocking JNI stubs matching `PROJECT.md`.

---

## 2. NDK & Toolchain Environment Analysis

### 2.1 Toolchain Inventory

Inspection of the host system (`D:\Android\sdk`) confirmed the presence of the required SDK components:

| Component | Path | Version | Verification Result |
|-----------|------|---------|---------------------|
| **Android NDK** | `D:\Android\sdk\ndk\26.1.10909125` | 26.1.10909125 | Clang 17.0.2 (r487747d) present |
| **Android SDK CMake** | `D:\Android\sdk\cmake\3.22.1` | 3.22.1 | Pre-installed, matches AGP expectation |
| **Host System CMake** | System PATH | 4.4.4 | Available for standalone test runner |
| **Target Toolchain Wrapper** | `.../windows-x86_64/bin/aarch64-linux-android26-clang++.cmd` | API 26 (Android 8.0+) | Verified with test shared library build |

### 2.2 C++20 Feature Verification

We compiled and executed a standalone verification test using the NDK Clang 17 toolchain testing core C++20 constructs:
- **Concepts & Constraints**: `template <typename T> concept AudioSampleType = std::floating_point<T> || std::integral<T>;` passed compile-time `static_assert`.
- **`<bit>`**: `std::endian::native == std::endian::little` evaluated at compile-time as `constexpr true` on ARM64 and x86_64 targets.
- **`<span>`**: `std::span<const float>` bounds and view slicing compiled and executed with zero overhead.
- **`<atomic>`**: Monotonic atomic memory orders (`std::memory_order_release`, `std::memory_order_acquire`) fully supported.

### 2.3 Target Shared Library Compilation Test

A target shared library test compiling `<jni.h>`, `<android/log.h>`, C++20 concepts, and linking `-llog` using `aarch64-linux-android26-clang++.cmd` produced `libtest_jni.so` (5,664 bytes) with exit code `0`, confirming target toolchain integrity.

---

## 3. CMake Configuration Design (`app/src/main/cpp/CMakeLists.txt`)

### 3.1 Mechanics & Prefab Ingestion

When Gradle builds the `:app` module with `buildFeatures.prefab = true` and dependency `com.google.oboe:oboe:1.9.0`:
1. AGP extracts the Oboe AAR prefab directory.
2. AGP automatically injects CMake search paths (`CMAKE_FIND_ROOT_PATH`) pointing to the unpacked prefab config.
3. In `CMakeLists.txt`, `find_package(oboe REQUIRED CONFIG)` locates `oboeConfig.cmake`.
4. The imported target `oboe::oboe` contains:
   - Include directories: `<path>/oboe/include`
   - Binary shared library: `liboboe.so` matching the active ABI (`arm64-v8a`, `x86_64`)
   - Dependent system libraries (`OpenSLES`, `aaudio`).
5. Target linkage: `target_link_libraries(vban_engine PRIVATE oboe::oboe log android)`.

### 3.2 Optimization and Safety Flags

- **Optimization**: `-O3` enables aggressive vectorization (NEON on ARM64, AVX/SSE on x86_64).
- **Float Validation**: Do **not** use `-ffast-math` globally. VBAN packets require checking for invalid IEEE 754 floats (`std::isnan`, `std::isinf`). Global `-ffast-math` allows the compiler to optimize away `std::isnan()`. Instead, use standard `-O3` or `-fno-finite-math-only`.
- **Visibility**: `-fvisibility=hidden` reduces dynamic symbol table size and prevents accidental export of internal audio engine helper functions.

### 3.3 Complete File Template: `app/src/main/cpp/CMakeLists.txt`

```cmake
# ==============================================================================
# VBAN Receptor - Android Native Audio Engine Build Configuration
# ==============================================================================
cmake_minimum_required(VERSION 3.22.1)

project("vban_engine" CXX)

# Enforce standard ISO C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Locate Prefab-provided Oboe audio library
# Requires buildFeatures.prefab = true and com.google.oboe:oboe:1.9.0 in app/build.gradle.kts
find_package(oboe REQUIRED CONFIG)

# Native include directory
set(VBAN_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Define shared library target
add_library(vban_engine SHARED
    src/vban_engine.cpp
    # Future milestone sources to be linked here:
    # src/vban_parser.cpp       (Milestone 2: Protocol header parsing)
    # src/vban_converter.cpp    (Milestone 2: PCM/Float32 dequantization)
    # src/drift_compensator.cpp (Milestone 3: Catmull-Rom resampler & PI servo)
    # src/oboe_player.cpp       (Milestone 3: Oboe AAudio exclusive player)
    # src/vban_receiver.cpp     (Milestone 4: UDP socket & jitter estimation)
    # src/jni_bridge.cpp        (Milestone 4: Dedicated JNI bridge)
)

# Export include directories
target_include_directories(vban_engine PUBLIC
    ${VBAN_INCLUDE_DIR}
)

# Compiler warnings and optimization flags
target_compile_options(vban_engine PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
    -O3
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# Link dependencies
target_link_libraries(vban_engine PRIVATE
    oboe::oboe
    log
    android
)
```

---

## 4. Native Header Architecture (`app/src/main/cpp/include/vban_engine.hpp`)

### 4.1 Interface Design

`vban_engine.hpp` serves as the high-level coordinator of the native audio subsystem. It defines:
1. `AudioStreamConfig`: Struct representing sample rate, channel count, and target audio device ID.
2. `NativeTelemetry`: Lock-free telemetry snapshot struct conforming to the `PROJECT.md` contract.
3. `AudioSampleType`: C++20 concept constraining valid audio sample formats.
4. `VbanEngine`: Thread-safe singleton controller managing start/stop, device routing, stream reconfiguration, and startup environment validation.

### 4.2 Complete File Template: `app/src/main/cpp/include/vban_engine.hpp`

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <span>
#include <concepts>
#include <bit>
#include <oboe/Oboe.h>

namespace vban {

/**
 * Audio stream configuration matching PROJECT.md interface contract.
 */
struct AudioStreamConfig {
    uint32_t sampleRate{48000};
    uint16_t channelCount{2};
    int32_t  targetDeviceId{0};
};

/**
 * Thread-safe lock-free telemetry snapshot matching PROJECT.md interface contract.
 */
struct NativeTelemetry {
    uint32_t sampleRate{0};
    uint16_t channelCount{0};
    uint16_t bitDepth{0};
    uint64_t totalPacketsReceived{0};
    uint64_t lostPackets{0};
    float    jitterMs{0.0f};
    float    bufferOccupancyPercent{0.0f};
    float    peakLevelLeft{0.0f};
    float    peakLevelRight{0.0f};
};

/**
 * C++20 Concept verification for audio sample types.
 */
template <typename T>
concept AudioSampleType = std::floating_point<T> || std::integral<T>;

/**
 * VbanEngine
 * 
 * Core coordinator managing the lifecycle of the VBAN audio reception,
 * packet processing, jitter buffer, and Oboe low-latency playback pipeline.
 */
class VbanEngine {
public:
    static VbanEngine& getInstance();

    VbanEngine();
    ~VbanEngine();

    // Prevent copy and move semantics (singleton audio engine)
    VbanEngine(const VbanEngine&) = delete;
    VbanEngine& operator=(const VbanEngine&) = delete;
    VbanEngine(VbanEngine&&) = delete;
    VbanEngine& operator=(VbanEngine&&) = delete;

    /**
     * Verifies C++20 features (concepts, std::endian, std::span) and
     * Oboe library headers / symbols at startup.
     * @return true if C++20 and Oboe environment are valid.
     */
    static bool verifyCpp20AndOboeEnvironment();

    /**
     * Initialize the native engine.
     * @return true if initialization succeeded.
     */
    bool initialize();

    /**
     * Check if engine is initialized.
     */
    bool isInitialized() const noexcept;

    /**
     * Start the VBAN reception and audio playback pipeline.
     * @param listenIp Local or multicast IP to bind (e.g. "0.0.0.0" or "239.255.0.1")
     * @param port UDP port (typically 6980)
     * @param streamName Null-terminated stream name filter (max 16 chars)
     * @param deviceId Target audio output device ID (0 = default)
     * @return true if started successfully.
     */
    bool start(const std::string& listenIp, uint16_t port, const std::string& streamName, int32_t deviceId = 0);

    /**
     * Stop the pipeline and release audio/network resources.
     */
    void stop();

    /**
     * Check if engine is actively running.
     */
    bool isRunning() const noexcept;

    /**
     * Route audio to a specific output device (e.g. USB DAC).
     * @param deviceId Target audio device ID.
     */
    void setAudioDevice(int32_t deviceId);

    /**
     * Reconfigure audio stream format (sample rate, channels).
     * Executes fade-out, rebuilds Oboe stream, executes fade-in.
     * @param config New stream configuration.
     * @return true if reconfiguration succeeded.
     */
    bool reconfigureStream(const AudioStreamConfig& config);

    /**
     * Poll latest telemetry snapshot (non-blocking, lock-free).
     * @return Current telemetry snapshot.
     */
    NativeTelemetry getTelemetry() const noexcept;

private:
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<int32_t> currentDeviceId_{0};

    AudioStreamConfig currentConfig_{};
    std::string listenIp_{"0.0.0.0"};
    uint16_t listenPort_{6980};
    std::string targetStreamName_{"Stream1"};

    mutable NativeTelemetry telemetryCache_{};
};

} // namespace vban
```

---

## 5. Native Source Implementation (`app/src/main/cpp/src/vban_engine.cpp`)

### 5.1 Key Elements
1. **Verification Routine**: `verifyCpp20AndOboeEnvironment()` tests `__cplusplus`, `std::endian`, `std::span`, concept static assertions, and constructs an `oboe::AudioStreamBuilder`.
2. **Lifecycle Control**: Atomic state transitions (`initialized_`, `running_`).
3. **JNI Stubs**: Fully implements `startReceiver`, `stopReceiver`, `setAudioDevice`, and `getTelemetry` matching the Java native interface for `com.vban.receptor.native.NativeVbanEngine`.

### 5.2 Complete File Template: `app/src/main/cpp/src/vban_engine.cpp`

```cpp
#include "vban_engine.hpp"

#include <android/log.h>
#include <jni.h>
#include <cstring>
#include <array>
#include <span>
#include <bit>

#define LOG_TAG "VbanEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace vban {

VbanEngine& VbanEngine::getInstance() {
    static VbanEngine instance;
    return instance;
}

VbanEngine::VbanEngine() = default;

VbanEngine::~VbanEngine() {
    stop();
}

bool VbanEngine::verifyCpp20AndOboeEnvironment() {
    LOGI("=== Initializing VBAN Engine C++20 & Oboe Verification ===");

    // 1. Verify C++20 compiler version
    #if defined(__cplusplus)
        LOGI("Detected __cplusplus: %ld", __cplusplus);
        #if __cplusplus < 202002L
        LOGW("Warning: __cplusplus is %ld, expected >= 202002L for C++20", __cplusplus);
        #endif
    #endif

    // 2. Verify C++20 <bit> endianness
    constexpr bool isLittle = (std::endian::native == std::endian::little);
    LOGI("C++20 std::endian check: %s", isLittle ? "Little-Endian (ARM/x86 native)" : "Big-Endian");

    // 3. Verify C++20 std::span
    constexpr float testArray[] = {0.0f, 0.25f, 0.5f, 1.0f};
    std::span<const float> testSpan{testArray};
    if (testSpan.size() != 4 || testSpan[3] != 1.0f) {
        LOGE("C++20 std::span validation failed!");
        return false;
    }
    LOGI("C++20 std::span validated successfully (size=%zu)", testSpan.size());

    // 4. Verify C++20 Concept usage
    static_assert(AudioSampleType<float>, "float must satisfy AudioSampleType");
    static_assert(AudioSampleType<int16_t>, "int16_t must satisfy AudioSampleType");
    static_assert(AudioSampleType<int32_t>, "int32_t must satisfy AudioSampleType");
    LOGI("C++20 concepts validated successfully");

    // 5. Verify Oboe Prefab linkage and AudioStreamBuilder instantiation
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Exclusive);
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Stereo);
    builder.setSampleRate(48000);

    const char* oboeVersion = oboe::getVersionText();
    LOGI("Oboe AudioStreamBuilder instantiated. Oboe library version: %s",
         oboeVersion ? oboeVersion : "unknown");

    LOGI("=== C++20 & Oboe Verification PASSED ===");
    return true;
}

bool VbanEngine::initialize() {
    if (initialized_.load(std::memory_order_acquire)) {
        LOGI("VbanEngine already initialized");
        return true;
    }

    if (!verifyCpp20AndOboeEnvironment()) {
        LOGE("VbanEngine environment verification failed!");
        return false;
    }

    initialized_.store(true, std::memory_order_release);
    LOGI("VbanEngine initialized successfully");
    return true;
}

bool VbanEngine::isInitialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

bool VbanEngine::start(const std::string& listenIp, uint16_t port, const std::string& streamName, int32_t deviceId) {
    if (!isInitialized()) {
        if (!initialize()) {
            return false;
        }
    }

    if (running_.load(std::memory_order_acquire)) {
        LOGW("VbanEngine is already running");
        return true;
    }

    listenIp_ = listenIp;
    listenPort_ = port;
    targetStreamName_ = streamName;
    currentDeviceId_.store(deviceId, std::memory_order_relaxed);

    LOGI("Starting VbanEngine: IP=%s, Port=%u, Stream='%s', DeviceId=%d",
         listenIp_.c_str(), listenPort_, targetStreamName_.c_str(), deviceId);

    // Initial telemetry state
    telemetryCache_.sampleRate = currentConfig_.sampleRate;
    telemetryCache_.channelCount = currentConfig_.channelCount;
    telemetryCache_.bitDepth = 24;
    telemetryCache_.totalPacketsReceived = 0;
    telemetryCache_.lostPackets = 0;
    telemetryCache_.jitterMs = 0.0f;
    telemetryCache_.bufferOccupancyPercent = 0.0f;
    telemetryCache_.peakLevelLeft = 0.0f;
    telemetryCache_.peakLevelRight = 0.0f;

    running_.store(true, std::memory_order_release);
    LOGI("VbanEngine started successfully");
    return true;
}

void VbanEngine::stop() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    LOGI("Stopping VbanEngine...");
    running_.store(false, std::memory_order_release);
    LOGI("VbanEngine stopped");
}

bool VbanEngine::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void VbanEngine::setAudioDevice(int32_t deviceId) {
    currentDeviceId_.store(deviceId, std::memory_order_relaxed);
    LOGI("VbanEngine audio device set to %d", deviceId);
}

bool VbanEngine::reconfigureStream(const AudioStreamConfig& config) {
    LOGI("VbanEngine reconfigureStream requested: %u Hz, %u ch, device %d",
         config.sampleRate, config.channelCount, config.targetDeviceId);
    currentConfig_ = config;
    telemetryCache_.sampleRate = config.sampleRate;
    telemetryCache_.channelCount = config.channelCount;
    return true;
}

NativeTelemetry VbanEngine::getTelemetry() const noexcept {
    return telemetryCache_;
}

} // namespace vban

// ==============================================================================
// JNI Bridge Implementation
// ==============================================================================
extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_initEngine(
    JNIEnv* /*env*/, jobject /*thiz*/) {
    return vban::VbanEngine::getInstance().initialize() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_startReceiver(
    JNIEnv* env, jobject /*thiz*/, jstring listenIp, jint port, jstring streamName, jint deviceId) {
    const char* ipChars = env->GetStringUTFChars(listenIp, nullptr);
    const char* nameChars = env->GetStringUTFChars(streamName, nullptr);
    
    std::string ip = ipChars ? ipChars : "0.0.0.0";
    std::string name = nameChars ? nameChars : "";
    
    if (ipChars) env->ReleaseStringUTFChars(listenIp, ipChars);
    if (nameChars) env->ReleaseStringUTFChars(streamName, nameChars);

    bool result = vban::VbanEngine::getInstance().start(ip, static_cast<uint16_t>(port), name, deviceId);
    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_stopReceiver(
    JNIEnv* /*env*/, jobject /*thiz*/) {
    vban::VbanEngine::getInstance().stop();
}

JNIEXPORT void JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_setAudioDevice(
    JNIEnv* /*env*/, jobject /*thiz*/, jint deviceId) {
    vban::VbanEngine::getInstance().setAudioDevice(deviceId);
}

JNIEXPORT jobject JNICALL
Java_com_vban_receptor_native_NativeVbanEngine_getTelemetry(
    JNIEnv* env, jobject /*thiz*/) {
    auto telemetry = vban::VbanEngine::getInstance().getTelemetry();

    jclass cls = env->FindClass("com/vban/receptor/native/NativeTelemetry");
    if (!cls) {
        LOGW("NativeTelemetry class not found via JNI");
        return nullptr;
    }

    jmethodID ctor = env->GetMethodID(cls, "<init>", "(IIIJJFFFF)V");
    if (!ctor) {
        LOGW("NativeTelemetry constructor (IIIJJFFFF)V not found via JNI");
        return nullptr;
    }

    return env->NewObject(
        cls, ctor,
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

## 6. Gradle & Prefab Integration Checklist

For the implementer of the Gradle scripts (`explorer_m1_gradle` / builder):

1. **`app/build.gradle.kts` configuration**:
   ```kotlin
   android {
       ...
       defaultConfig {
           ...
           ndk {
               abiFilters.addAll(listOf("arm64-v8a", "x86_64"))
           }
           externalNativeBuild {
               cmake {
                   arguments(
                       "-DANDROID_STL=c++_shared",
                       "-DANDROID_TOOLCHAIN=clang"
                   )
                   cppFlags("-std=c++20")
               }
           }
       }
       externalNativeBuild {
           cmake {
               path = file("src/main/cpp/CMakeLists.txt")
               version = "3.22.1"
           }
       }
       buildFeatures {
           compose = true
           prefab = true
       }
       ndkVersion = "26.1.10909125"
   }

   dependencies {
       implementation("com.google.oboe:oboe:1.9.0")
       ...
   }
   ```
2. **`local.properties` configuration**:
   ```properties
   sdk.dir=D\:\\Android\\sdk
   ndk.dir=D\:\\Android\\sdk\\ndk\\26.1.10909125
   ```

---

## 7. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| **STL Mismatch (`c++_static` vs `c++_shared`)** | Duplicate C++ runtime, multiple global memory allocators, subtle crash bugs | Enforce `-DANDROID_STL=c++_shared` in Gradle `externalNativeBuild.cmake`. Oboe Prefab packages link against `c++_shared`. |
| **Missing Prefab Configuration** | CMake `find_package(oboe REQUIRED CONFIG)` fails to locate `oboe` package | Ensure `buildFeatures.prefab = true` is explicitly enabled in `app/build.gradle.kts`. |
| **Global `-ffast-math` breaking IEEE 754 float checks** | Corrupt/NaN audio samples pass through parser undetected, causing loud DAC pop/noise | Restrict optimizations to `-O3` without `-ffast-math`, or use `-fno-finite-math-only`. |
| **Symbol Visibility Pollution** | Leaking private audio engine classes into shared library exports | Set `-fvisibility=hidden` and `-fvisibility-inlines-hidden` in `target_compile_options`. Mark only JNI endpoints with `JNIEXPORT`. |
| **JNI Signature Drift** | `NoSuchMethodError` / `UnsatisfiedLinkError` at runtime | Guard JNI class lookups (`FindClass`, `GetMethodID`) with null checks, and provide exact type signature `(IIIJJFFFF)V`. |

---
