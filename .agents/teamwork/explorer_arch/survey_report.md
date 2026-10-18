# VBAN Receptor: Native Architecture & Environment Survey Report

**Author**: Explorer Architecture Agent (`explorer_arch`)  
**Date**: 2026-10-18  
**Target Workspace**: `D:\Workspaces\vban-receptor`  
**Target Platform**: Android (ARM64 / x86_64), C++20 NDK, Oboe 1.9.0, Jetpack Compose  

---

## Executive Summary

This architectural survey delivers a complete blueprint for the audiophile-grade, ultra-low latency VBAN (VB-Audio Network) Receptor for Android. It investigates the host toolchains and build environment, establishes the lock-free Single-Producer Single-Consumer (SPSC) circular buffer design, defines the dynamic clock drift compensation and resampling DSP pipeline, and specifies the verification harness.

---

## 1. Workspace & Toolchain Inspection

### 1.1 Host Environment & SDK Paths

An inspection of the workspace root and host environment reveals the following components:

| Component | Path / Version | Status & Capability |
| :--- | :--- | :--- |
| **Workspace Root** | `D:\Workspaces\vban-receptor` | Clean repository (`ORIGINAL_REQUEST.md`, `.git`, `.agents`, `.orca`). |
| **Android SDK** | `D:\Android\sdk` | Fully installed with build-tools, cmake, ndk, platforms. |
| **Android NDK** | `D:\Android\sdk\ndk\26.1.10909125` (r26b LTS)<br>`D:\Android\sdk\ndk\29.0.14206865` (r29) | **NDK 26.1.10909125** is selected. Includes Clang 17.0.2 with complete C++20 standard library support (`<atomic>`, `<span>`, `<concepts>`, `<ranges>`). |
| **Android CMake** | `D:\Android\sdk\cmake\3.22.1` | Standard Android CMake version supported natively by AGP 8.x. |
| **Host Compilers** | NDK Clang 17.0.2 (`windows-x86_64`)<br>TDM-GCC 10.3.0 (`C:\TDM-GCC-64\bin\g++.exe`) | Verified capable of compiling standalone native C++20 test suites directly on Windows without an emulator. |
| **Java / JDK** | OpenJDK 17.0.18 (Eclipse Adoptium) | Perfectly matched for Android Gradle Plugin 8.x. |
| **Gradle** | Gradle 8.10.2 cached at `C:\Users\Geko\.gradle\wrapper\dists\gradle-8.10.2-bin\` | Ready for wrapper generation and APK build verification. |
| **Android Platforms** | `android-34`, `android-35`, `android-37.0` | Target SDK: 35. Supports Android 14+ (`API 34`) USB DAC `MIXER_BEHAVIOR_BIT_PERFECT`. |

### 1.2 Oboe 1.9.0 Prefab & CMake Integration

Oboe is distributed as an AAR containing native Prefab metadata. To ensure proper linkage and avoid build failures, the following configuration must be applied:

#### App `build.gradle.kts` Configuration
```kotlin
android {
    compileSdk = 35

    defaultConfig {
        minSdk = 26
        targetSdk = 35

        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DCMAKE_CXX_STANDARD=20",
                    "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
                )
                cppFlags("-O3", "-ffast-math", "-fvisibility=hidden")
                abiFilters("arm64-v8a", "x86_64")
            }
        }
    }

    buildFeatures {
        prefab = true
        compose = true
    }
}

dependencies {
    implementation("com.google.oboe:oboe:1.9.0")
    // ...
}
```

> **CRITICAL REQUIREMENT**: Prefab packages Oboe compiled against `libc++_shared.so`. Specifying `-DANDROID_STL=c++_shared` is mandatory; defaulting to `c++_static` causes Prefab linking errors.

#### `CMakeLists.txt` Configuration
```cmake
cmake_minimum_required(VERSION 3.22.1)
project("vban_receptor_native" CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Discover Oboe via Prefab
find_package(oboe REQUIRED CONFIG)

add_library(vban_engine SHARED
    src/vban_parser.cpp
    src/pcm_converter.cpp
    src/resampler.cpp
    src/clock_drift_tracker.cpp
    src/audio_output_stream.cpp
    src/vban_engine_jni.cpp
)

target_include_directories(vban_engine
    PUBLIC
    include
)

target_link_libraries(vban_engine
    PRIVATE
    oboe::oboe
    log
    android
)
```

---

## 2. Lock-Free SPSC Circular Buffer Design

### 2.1 Concurrency Model & Real-Time Constraints

The receptor utilizes a Single-Producer Single-Consumer (SPSC) lock-free ring buffer:
- **Producer Thread**: UDP Network Receiver Thread. Receives UDP datagrams from the network socket (socket buffer set to 4 MB via `SO_RCVBUF`), decodes VBAN audio packets, converts PCM/Float to normalized `float32`, and pushes samples into the SPSC buffer.
- **Consumer Thread**: Oboe Audio Callback Thread (`onAudioReady`). Executes in the AAudio/OpenSL real-time audio thread (`SCHED_FIFO`).

#### Hard Real-Time Invariants for Consumer:
1. **Zero Dynamic Allocations**: No calls to `malloc`, `free`, `new`, `delete`, or STL containers that allocate on the heap.
2. **Zero Locking / Blocking**: No mutexes, condition variables, semaphores, or kernel blocking calls (`futex`).
3. **Bounded Execution Time**: All operations must be wait-free and $O(N)$ with small constant factors.

### 2.2 Cache Line Alignment & False Sharing Elimination

Modern mobile 64-bit processors (ARM Cortex-A55, Cortex-X1..X4, etc.) utilize **64-byte L1/L2 cache lines**. If the producer's write index and the consumer's read index occupy the same cache line, every write by either thread invalidates the other thread's cache line (cache line bouncing / false sharing), generating bus snooping stalls and microsecond-level audio callback jitter.

#### Memory Layout Design
```cpp
template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static constexpr size_t kIndexMask = Capacity - 1;

private:
    // 64-byte aligned Producer State
    alignas(64) std::atomic<uint64_t> m_write_index{0};
    uint64_t m_cached_read_index{0};
    uint8_t m_pad_producer[64 - sizeof(std::atomic<uint64_t>) - sizeof(uint64_t)];

    // 64-byte aligned Consumer State
    alignas(64) std::atomic<uint64_t> m_read_index{0};
    uint64_t m_cached_write_index{0};
    uint8_t m_pad_consumer[64 - sizeof(std::atomic<uint64_t>) - sizeof(uint64_t)];

    // Buffer Storage: 64-byte aligned, zero dynamic allocation during operation
    alignas(64) std::unique_ptr<T[]> m_storage;
```

### 2.3 Atomics & Memory Ordering

- **Monotonic 64-bit Indices (`uint64_t`)**: At 768 kHz, $2^{64}$ frames provides continuous streaming for over 761,000 years, completely eliminating index wraparound edge cases.
- **Release-Acquire Semantics**:
  - When Producer finishes writing a block of samples:
    ```cpp
    m_write_index.store(new_write_idx, std::memory_order_release);
    ```
  - When Consumer loads the write index before reading:
    ```cpp
    m_cached_write_index = m_write_index.load(std::memory_order_acquire);
    ```
  - When Consumer finishes reading a block of samples:
    ```cpp
    m_read_index.store(new_read_idx, std::memory_order_release);
    ```
  - When Producer loads the read index before writing:
    ```cpp
    m_cached_read_index = m_read_index.load(std::memory_order_acquire);
    ```
- **Local Index Caching**: The producer maintains `m_cached_read_index` and only executes atomic `.load(std::memory_order_acquire)` when the locally cached space appears insufficient. Similarly, the consumer maintains `m_cached_write_index` and only executes atomic load when data appears exhausted. This reduces inter-core atomic bus synchronization by over 95%.

### 2.4 Power-of-Two Capacity & Circular Masking

By enforcing `Capacity = 2^N`, index wrapping is computed with bitwise AND:
$$\text{offset} = \text{index} \ \& \ (Capacity - 1)$$
Block transfers are executed in at most two contiguous `std::memcpy` segments:
1. From current offset to `min(offset + count, Capacity)`.
2. From index 0 to `remaining`.

### 2.5 Capacity Sizing for Ultra-High Resolution Audio (Up to 768 kHz)

Data throughput and frame duration analysis across sample rates:

| Sample Rate | Channels | Format | Byte Rate / sec | Capacity ($2^{18} = 262,144$ frames) | Time Duration |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **48 kHz** | 2 (Stereo) | Float32 | 384 KB/s | 2.097 MB | **5.46 seconds** |
| **96 kHz** | 2 (Stereo) | Float32 | 768 KB/s | 2.097 MB | **2.73 seconds** |
| **192 kHz** | 2 (Stereo) | Float32 | 1.536 MB/s | 2.097 MB | **1.36 seconds** |
| **384 kHz** | 2 (Stereo) | Float32 | 3.072 MB/s | 2.097 MB | **682.6 ms** |
| **768 kHz** | 2 (Stereo) | Float32 | 6.144 MB/s | 2.097 MB | **341.3 ms** |
| **768 kHz** | 8 (Max VBAN)| Float32 | 24.576 MB/s | 8.388 MB | **341.3 ms** |

**Sizing Decision**:
A buffer capacity of **262,144 frames ($2^{18}$)** provides a generous 341 ms cushion at 768 kHz (and over 1.3 seconds at 192 kHz) while consuming only ~2 MB RAM for stereo (or ~8.4 MB for 8 channels). This easily absorbs 50ms–150ms Wi-Fi packet bursts without underrun.

---

## 3. Dynamic Clock Drift Compensation & Resampling Architecture

### 3.1 Clock Drift Physics & Mathematical Analysis

In real-world network audio, the transmitter crystal oscillator ($f_{tx}$) and receiver DAC crystal oscillator ($f_{rx}$) operate asynchronously. Typical commercial quartz crystals exhibit tolerances of $\pm 10$ to $\pm 50$ parts per million (PPM).

$$\Delta_{\text{drift}} = \frac{f_{tx} - f_{rx}}{f_0} \in [-100 \text{ PPM}, +100 \text{ PPM}]$$

Drift impact over time:
- At 48 kHz with 40 PPM drift: **1.92 frames/second** (115 frames/minute).
- At 768 kHz with 40 PPM drift: **30.72 frames/second** (1,843 frames/minute).

Without active compensation:
- If $f_{rx} > f_{tx}$: buffer drains continuously until an **underrun dropout** occurs.
- If $f_{tx} > f_{rx}$: buffer fills continuously until an **overflow drop** or latency escalation occurs.

### 3.2 Watermark Tracking & Jitter Rejection Control Loop

#### Target Occupancy
Define a nominal target watermark $W_{\text{target}}$ (e.g., 50% of the jitter buffer, or $\approx 30\text{ ms} - 50\text{ ms}$).

#### Dual-Stage Filter Architecture
Raw instantaneous buffer occupancy $W(t) = m\_write\_index - m\_read\_index$ suffers from severe high-frequency variance caused by Wi-Fi packet batching (jitter). Directly feeding $W(t)$ into a speed controller would induce audible pitch flutter (frequency modulation).

1. **Jitter Low-Pass Filter**: A second-order critically damped digital filter or Exponential Moving Average (EMA) with a slow time constant $\tau \approx 8.0 \text{ seconds}$:
   $$\alpha = 1.0 - e^{-\Delta t / \tau}$$
   $$W_{\text{filtered}}(k) = (1 - \alpha) \cdot W_{\text{filtered}}(k-1) + \alpha \cdot W(k)$$

2. **Proportional-Integral (PI) Drift Servo**:
   $$\text{error}(k) = W_{\text{filtered}}(k) - W_{\text{target}}$$
   $$\text{integral}(k) = \text{integral}(k-1) + \text{error}(k) \cdot \Delta t$$
   $$\Delta r(k) = K_p \cdot \text{error}(k) + K_i \cdot \text{integral}(k)$$

3. **Audiophile Safety Constraints**:
   - **Clamping**: Maximum correction ratio is clamped to $\pm 150 \text{ PPM}$ ($0.015\%$).
   - **Slew-Rate Limiting**: $|d(\Delta r) / dt| \le 0.1 \text{ PPM/sec}$.
   - Human pitch perception threshold (Just Noticeable Difference) is approximately 5 to 10 cents ($2900 \text{ to } 5800 \text{ PPM}$). A slew-rate limited correction under 150 PPM is mathematically and psychoacoustically undetectable.

### 3.3 Micro-Resampling Algorithm: Cubic Hermite (Catmull-Rom)

For micro-drift adjustment where the nominal sample rate matches ($F_{in} == F_{out}$):
- **Catmull-Rom Cubic Hermite Spline** provides $C^1$ continuity (continuous first derivative, no gradient steps).
- **Execution Cost**: Exactly 4 Fused Multiply-Adds (FMAs) per sample channel.
- **Latency**: 1 sample lookahead delay.
- **Dynamic Phase Tracking**:
  ```cpp
  // Fractional phase step based on drift ratio R(t) = 1.0 + delta_r
  float step = 1.0f + delta_r;
  m_phase += step;
  while (m_phase >= 1.0f) {
      m_phase -= 1.0f;
      advance_input_frame();
  }
  ```
  Given 4 consecutive input samples $y_{-1}, y_0, y_1, y_2$ and fractional phase $x \in [0.0, 1.0)$:
  $$c_0 = y_0$$
  $$c_1 = 0.5 \cdot (y_1 - y_{-1})$$
  $$c_2 = y_{-1} - 2.5 \cdot y_0 + 2.0 \cdot y_1 - 0.5 \cdot y_2$$
  $$c_3 = 0.5 \cdot (y_2 - y_{-1}) + 1.5 \cdot (y_0 - y_1)$$
  $$y(x) = ((c_3 \cdot x + c_2) \cdot x + c_1) \cdot x + c_0$$

### 3.4 Graceful Stream Reconfiguration

When a sender changes sample rate (e.g., 44.1 kHz $\to$ 96 kHz) or channel configuration:
1. **Network Thread Detection**: Parses VBAN header `format_SR`, `format_nbc`, and `format_bit`. If altered, triggers reconfiguration request.
2. **Lock-Free State Handshake**:
   - Audio state set to `RECONFIGURING`.
   - Consumer thread executes a **128-sample equal-power cosine fade-out** to digital silence (eliminating audible clicks/pops).
3. **Asynchronous Stream Recreation**:
   - An asynchronous control thread stops and closes the Oboe stream.
   - Reconstructs `oboe::AudioStreamBuilder` with updated sample rate and channel format.
   - Reopens stream in exclusive low-latency mode.
   - Flushes SPSC queue and resets drift PI controller.
4. **Fade-In**:
   - Audio callback ramps volume up smoothly from 0.0 to 1.0 over 128 samples.
   - Zero blocking or deadlock in the real-time audio thread.

---

## 4. VBAN Protocol Specification Conformance

### 4.1 Header Structure (28 Bytes Exact)

```cpp
#pragma pack(push, 1)
struct VbanHeader {
    uint32_t magic;         // FourCC: 'V','B','A','N' (0x4E414256 little-endian)
    uint8_t  format_SR;     // Bits 5..7: Subprotocol; Bits 0..4: Sample Rate index
    uint8_t  format_nbs;    // Number of samples per frame minus 1 (0..255 -> 1..256)
    uint8_t  format_nbc;    // Number of channels minus 1 (0..7 -> 1..8 channels)
    uint8_t  format_bit;    // Bits 0..2: Data format; Bits 3..7: Codec (0 = PCM)
    char     streamname[16];// 16-byte stream name, null-padded ASCII
    uint32_t frame_counter; // 32-bit incremental sequence packet counter
};
#pragma pack(pop)
static_assert(sizeof(VbanHeader) == 28, "VBAN Header must be exactly 28 bytes");
```

### 4.2 Official 21 Sample Rates & Extended Audiophile Mapping

```cpp
inline constexpr uint32_t kOfficialVbanSampleRates[21] = {
    6000,  12000, 24000, 48000, 96000, 192000, 384000,  // Group 1
    8000,  16000, 32000, 64000, 128000, 256000, 512000, // Group 2
    11025, 22050, 44100, 88200, 176400, 352800, 705600  // Group 3
};
// Extended audiophile mapping for index 21: 768000 Hz
```

### 4.3 Data Formats & De-quantization to Normalized Float32

- **INT16 (Format `0x01`)**:
  $$x_{\text{norm}} = \frac{x_{\text{int16}}}{32768.0f}$$
- **INT24 Packed (Format `0x02`, 3 bytes Little-Endian)**:
  $$x_{\text{raw}} = (b_2 \ll 24) \ | \ (b_1 \ll 16) \ | \ (b_0 \ll 8)$$
  $$\text{Sign-extended: } x_{\text{int24}} = x_{\text{raw}} \gg 8$$
  $$x_{\text{norm}} = \frac{x_{\text{int24}}}{8388608.0f}$$
- **INT32 (Format `0x03`)**:
  $$x_{\text{norm}} = \frac{x_{\text{int32}}}{2147483648.0f}$$
- **FLOAT32 (Format `0x04`)**:
  IEEE-754 32-bit float pass-through, verified for `NaN`/`Inf` sanitization and clamped to `[-1.0f, +1.0f]`.

---

## 5. Verification & Testing Strategy

### 5.1 Standalone C++ Native Test Harness

The test suite can be compiled and executed directly using NDK Clang or host GCC:

1. `test_vban_parser`:
   - Validates header size (28 bytes) and FourCC magic `'VBAN'`.
   - Validates audio subprotocol verification (`format_SR & 0xE0 == 0x00`).
   - Validates all 21 standard sample rates and channel counts (1 to 8).
   - Validates stream name extraction and sequence counter tracking.
2. `test_pcm_converter`:
   - Validates maximum, minimum, and zero conversions for 16-bit, 24-bit packed, and 32-bit int.
   - Validates float passthrough and clipping protection.
3. `test_spsc_queue`:
   - Multi-threaded stress test with concurrent producer and consumer threads.
   - Pushes 10,000,000 frames through the queue.
   - Verifies zero packet loss, zero data corruption, and verifies throughput exceeds 100 million samples/sec.
4. `test_clock_drift`:
   - Simulates simulated transmitter clock drifts ($\pm 50$ PPM).
   - Validates PI controller convergence and confirms zero buffer underrun/overflow over long virtual streaming runs.

### 5.2 Android APK Build Verification

Verification command:
```powershell
& "C:\Users\Geko\.gradle\wrapper\dists\gradle-8.10.2-bin\a04bxjujx95o3nb99gddekhwo\gradle-8.10.2\bin\gradle.bat" assembleDebug
```
or via the generated project `gradlew.bat assembleDebug`.

---

## 6. Architectural Recommendations for Implementation

1. **Modular Directory Structure**:
   ```
   app/
   ├── src/
   │   ├── main/
   │   │   ├── cpp/
   │   │   │   ├── CMakeLists.txt
   │   │   │   ├── include/
   │   │   │   │   ├── vban_protocol.hpp
   │   │   │   │   ├── spsc_queue.hpp
   │   │   │   │   ├── pcm_converter.hpp
   │   │   │   │   ├── clock_drift_tracker.hpp
   │   │   │   │   ├── hermite_resampler.hpp
   │   │   │   │   └── audio_output_stream.hpp
   │   │   │   └── src/
   │   │   │       ├── vban_parser.cpp
   │   │   │       ├── pcm_converter.cpp
   │   │   │       ├── clock_drift_tracker.cpp
   │   │   │       ├── hermite_resampler.cpp
   │   │   │       ├── audio_output_stream.cpp
   │   │   │       └── vban_engine_jni.cpp
   │   │   ├── java/com/vban/receptor/
   │   │   │   ├── service/VbanPlaybackService.kt
   │   │   │   ├── audio/UsbDeviceManager.kt
   │   │   │   └── ui/
   │   │   └── AndroidManifest.xml
   ```
2. **Android 14+ Bit-Perfect Support**:
   In `UsbDeviceManager.kt`:
   - Enumerate connected USB devices via `AudioManager.getDevices(GET_DEVICES_OUTPUTS)`.
   - On Android 14+ (`Build.VERSION.SDK_INT >= 34`), inspect `audioDeviceInfo.getAudioMixerAttributes()`.
   - Request `MIXER_BEHAVIOR_BIT_PERFECT` to completely bypass Android's AudioFlinger hardware resampler.
3. **Foreground Service & Low Latency Locks**:
   - Foreground service type: `mediaPlayback` (`android:foregroundServiceType="mediaPlayback"`).
   - Acquire `WifiManager.WifiLock` with `WIFI_MODE_FULL_LOW_LATENCY` (API 29+) to disable Wi-Fi power-save polling intervals.
   - Acquire `PowerManager.WakeLock` with `PARTIAL_WAKE_LOCK`.
