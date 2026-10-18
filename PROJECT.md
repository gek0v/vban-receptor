# Project: VBAN Receptor for Android

## Architecture

The VBAN Receptor is a high-fidelity, audiophile-grade, low-latency audio receptor for Android, built with modern C++20 (Android NDK) and Kotlin (Jetpack Compose). It is architectured around a strict decoupled, zero-allocation real-time audio pipeline:

```
                          UDP Network (Port 6980)
                                    │
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Native Network Thread (vban_receiver)                                  │
│ - 4 MB SO_RCVBUF UDP Socket (Unicast & Multicast IP_ADD_MEMBERSHIP)    │
│ - 10-Step Header & Payload Packet Validation Pipeline                  │
│ - Monotonic nuFrame Tracking & RFC 3550 Jitter Estimation              │
│ - Zero-Allocation Audio Dequantization to Float32 [-1.0f, +1.0f]       │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Interleaved Float32 Audio Frames
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Lock-Free SPSC Circular Buffer (spsc_ring_buffer)                      │
│ - Capacity: 2^18 frames (262,144 frames = 341.3 ms at 768 kHz)         │
│ - Cache-line aligned (alignas(64)) read & write indices                │
│ - Monotonic uint64_t counters with std::memory_order_release / acquire │
│ - ZERO dynamic allocations or system calls during streaming            │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ High-Priority Audio Callback Thread
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Oboe Audio Engine & Clock Drift Compensator (oboe_player)              │
│ - Adaptive Clock Drift Servo: EMA Jitter Filter + PI Drift Controller  │
│ - Micro-Interpolation: Catmull-Rom Cubic Hermite Spline Resampling     │
│ - Oboe 1.9.0 AAudio Exclusive Mode (Up to 768 kHz, LowLatency)         │
│ - Decoupled Stream Reconfiguration with 128-sample Cosine Crossfade    │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │
                                    ▼
                 Hardware Audio HAL / External USB DAC

                                    ▲
                                    │ JNI Bridge (Lock-Free Telemetry)
┌───────────────────────────────────┴────────────────────────────────────┐
│ Kotlin / Android Framework Layer                                       │
│ - VbanPlaybackService (Foreground Service type="mediaPlayback")        │
│ - 3 Locks: WifiLock (WIFI_MODE_FULL_LOW_LATENCY), WakeLock, MulticastLock│
│ - Android 14+ (API 34) Bit-Perfect USB DAC Bypass                      │
│   (AudioManager.setPreferredMixerAttributes MIXER_BEHAVIOR_BIT_PERFECT)│
│ - Jetpack Compose Reactive Dashboard (VU Meter, Buffer Gauge, UI)     │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Feature Inventory

Every feature discovered during the Survey phase is mapped below. Every feature is assigned to a specific implementation milestone.

| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | NDK C++20 & CMake Scaffolding | Configure Gradle with NDK 26.1, CMake 3.22+, and C++20 flags | M1 | explorer_arch |
| 2 | Oboe 1.9.0 Prefab Linkage | Pull in Oboe 1.9.0 via Gradle Prefab and link `oboe::oboe` with `c++_shared` | M1 | explorer_arch / spec_miner_android |
| 3 | Standalone Native Test Harness | Host/NDK C++ test harness setup for fast unit testing | M1 | explorer_arch |
| 4 | Fixed 28-Byte Header Structure | `#pragma pack(push, 1)` struct layout exactly matching VBAN spec | M2 | spec_miner_vban |
| 5 | FourCC Magic Validation | Verify `'V', 'B', 'A', 'N'` (`0x4E414256` LE) magic bytes | M2 | spec_miner_vban |
| 6 | Subprotocol Verification | Extract bits 7..5 of `format_SR`; enforce `0x00` (Audio) | M2 | spec_miner_vban |
| 7 | Official 21 Sample Rates Table | Table lookup for rates 6 kHz to 705.6 kHz | M2 | spec_miner_vban |
| 8 | 768 kHz Sample Rate Extension | Support index 21 extension ($16 \times 48\text{ kHz}$) for audiophile DACs | M2 | spec_miner_vban |
| 9 | Frame Size (`format_nbs`) Decoding | Decode sample frames per packet (`format_nbs + 1`, 1..256) | M2 | spec_miner_vban |
| 10 | Channel Count (`format_nbc`) Decoding | Decode channels per frame (`format_nbc + 1`, 1..256) | M2 | spec_miner_vban |
| 11 | Codec & Reserved Bit 3 Check | Enforce PCM codec `0x00` and `format_bit & 0x08 == 0` | M2 | spec_miner_vban |
| 12 | Stream Name Matching | 16-byte null-padded stream name extraction and filtering | M2 | spec_miner_vban |
| 13 | Exact Payload Size Calculation | Verify received byte count == $28 + (\text{nbs}+1)(\text{nbc}+1) \times \text{sampleSize}$ | M2 | spec_miner_vban |
| 14 | PCM 16-bit Dequantization | Unpack 16-bit signed LE PCM to Float32 `[-1.0f, +1.0f]` | M2 | spec_miner_vban |
| 15 | PCM 24-bit Packed Dequantization | Unpack 3-byte packed signed LE PCM with sign extension to Float32 `[-1.0f, +1.0f]` | M2 | spec_miner_vban |
| 16 | PCM 32-bit Dequantization | Unpack 32-bit signed LE PCM to Float32 `[-1.0f, +1.0f]` | M2 | spec_miner_vban |
| 17 | Float32 IEEE 754 Normalization & Sanitization | Clamping and NaN/Inf replacement for 32-bit float audio | M2 | spec_miner_vban |
| 18 | Channel Deinterleaving / Interleaving | Standard channel ordering for multi-channel processing | M2 | spec_miner_vban |
| 19 | Lock-Free SPSC Circular Buffer | 64-byte aligned, $2^{18}$ frames capacity, atomic release/acquire, zero allocations | M2 | explorer_arch |
| 20 | Oboe Low-Latency Stream Setup | Exclusive mode, low-latency, AAudio backend, sample rates up to 768 kHz | M3 | spec_miner_android / explorer_arch |
| 21 | Real-Time Audio Callback Engine | Zero-allocation `AudioStreamDataCallback::onAudioReady` pulling from SPSC buffer | M3 | spec_miner_android / explorer_arch |
| 22 | Audio Error Recovery Callback | Asynchronous stream restart in `AudioStreamErrorCallback::onErrorAfterClose` | M3 | spec_miner_android |
| 23 | Clock Drift Jitter Filter | 2nd-order slow EMA filter tracking average buffer watermark | M3 | explorer_arch |
| 24 | Adaptive Clock Drift Servo | PI controller modulating playback rate within $\pm 100$ PPM (slew $\le 0.1$ PPM/s) | M3 | explorer_arch |
| 25 | Catmull-Rom Cubic Hermite Micro-Resampler | High-quality 4-point cubic Hermite micro-interpolation without audible clicks | M3 | explorer_arch |
| 26 | Decoupled Stream Reconfiguration | Safe stream format changes with 128-sample cosine fade-out/fade-in | M3 | explorer_arch |
| 27 | UDP Socket Network Engine | UDP socket bound to port 6980 with 4 MB `SO_RCVBUF` | M4 | spec_miner_vban / explorer_arch |
| 28 | UDP Multicast Group Joining | `IP_ADD_MEMBERSHIP` socket option handling | M4 | spec_miner_vban |
| 29 | Packet Sequence & Loss Tracking | `nuFrame` sequence checking with unsigned 32-bit wrap handling | M4 | spec_miner_vban |
| 30 | Inter-Arrival Jitter Estimation | RFC 3550 EWMA jitter estimation | M4 | spec_miner_vban |
| 31 | Non-Blocking JNI Bridge | JNI interface for engine control (start, stop, device ID) and telemetry poll | M4 | spec_miner_android |
| 32 | NativeVbanEngine Kotlin Wrapper | Kotlin wrapper managing native lifecycle and state flows | M4 | spec_miner_android |
| 33 | Android App & Compose Project | Application class, MainActivity, themes, and dependencies | M5 | spec_miner_android |
| 34 | Foreground Service (`mediaPlayback`) | Android 14 compliant foreground service with persistent notification | M5 | spec_miner_android |
| 35 | Triple Lock Power & Network Manager | `WifiLock` (WIFI_MODE_FULL_LOW_LATENCY), `WakeLock`, `MulticastLock` | M5 | spec_miner_android |
| 36 | Android Permissions Handling | Runtime permissions for `POST_NOTIFICATIONS`, foreground service types | M5 | spec_miner_android |
| 37 | Android 14+ Bit-Perfect USB DAC Bypass | `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT` via `setPreferredMixerAttributes` | M5 | spec_miner_android |
| 38 | Android 13- AAudio Fallback | Graceful fallback to AAudio exclusive mode on older Android releases | M5 | spec_miner_android |
| 39 | Audio Device Enumeration & Selection | USB DAC, headphones, speaker detection with dynamic hotplug callback | M5 | spec_miner_android |
| 40 | Jetpack Compose Dashboard UI | Reactive control screen (Start/Stop, IP, Port, Stream Name, Device Selector) | M5 | spec_miner_android |
| 41 | 60 FPS Stereo VU Meter Canvas | Real-time decibel scale (-60 dBFS to 0 dBFS) with peak hold decay | M5 | spec_miner_android |
| 42 | Jitter & Buffer Occupancy Gauge | Visual buffer fill level indicator and Wi-Fi jitter metric | M5 | spec_miner_android |
| 43 | Real-Time Telemetry Counters | Live display of detected sample rate, bit depth, channels, and packet loss | M5 | spec_miner_android |
| 44 | E2E Test Suite Execution (Tiers 1-4) | Pass 100% of requirement-driven E2E tests published in TEST_READY.md | M6 | project_pattern |
| 45 | Adversarial Coverage Hardening (Tier 5) | White-box stress-testing and edge-case bug hunting by Challenger agents | M6 | project_pattern |

---

## Milestones

| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Build Scaffolding & Native Test Harness | Gradle configuration, NDK 26.1, CMake 3.22+, Oboe 1.9.0 Prefab, Standalone C++ test runner setup | none | DONE |
| M2 | Native VBAN Core (Protocol, Formats & SPSC) | VBAN header parser, 21 rates + 768k, dequantizers (16/24/32/float), lock-free SPSC circular buffer, native unit tests | M1 | DONE |
| M3 | Oboe Audio Engine, Clock Drift & Resampler | Oboe 1.9.0 low-latency player (up to 768 kHz), EMA + PI clock drift compensator, Catmull-Rom micro-resampler, stream reconfiguration | M2 | DONE |
| M4 | Network Receiver & JNI Telemetry Bridge | UDP socket receiver (port 6980, 4MB buffer, multicast), RFC 3550 jitter & loss tracking, JNI interface & Kotlin wrapper | M2, M3 | DONE |
| M5 | Android Service, Bit-Perfect DAC & Compose UI | `mediaPlayback` Foreground Service, 3-lock manager, Android 14+ bit-perfect USB mixer bypass, Jetpack Compose UI (VU meter, buffer gauge, controls) | M4 | DONE |
| M6 | Final Milestone (E2E Pass & Adversarial Hardening) | Phase 1: Pass 100% of E2E tests (Tiers 1-4). Phase 2: Tier 5 adversarial coverage hardening | M5, TEST_READY.md | DONE |

---

## Interface Contracts

### M1 ↔ M2, M3, M4: CMake & Compiler Environment
- C++ Standard: C++20 (`-std=c++20`).
- Memory Model: 64-bit ARM (`arm64-v8a`) and x86_64 (`x86_64`).
- Standard Library: `c++_shared` (LLVM libc++).
- Oboe Target: `oboe::oboe` linked via `find_package(oboe REQUIRED CONFIG)`.

### M2 (VBAN Core) ↔ M3 (Oboe Player): SPSC Ring Buffer Contract
- Type: `SpscRingBuffer<float, 262144>` (Interleaved Float32 samples).
- Producer: Network worker thread calls `write(const float* data, size_t frames) -> size_t`.
- Consumer: Oboe audio callback calls `read(float* outData, size_t frames) -> size_t`.
- Real-time constraint: Neither `write` nor `read` may block, allocate memory, or invoke system calls.
- Memory ordering: Read index and Write index synchronized via `std::memory_order_release` and `std::memory_order_acquire`.
- Available frames query: `size_t availableRead() const noexcept` (for drift compensator watermark estimation).

### M3 (Audio Engine) ↔ M4 (Network Receiver): Stream Configuration Contract
- Struct:
  ```cpp
  struct AudioStreamConfig {
      uint32_t sampleRate;
      uint16_t channelCount;
      int32_t  targetDeviceId;
  };
  ```
- Reconfiguration: `VbanEngine::reconfigureStream(const AudioStreamConfig& config)` triggers a 128-sample cosine fade-out, signals worker thread, re-creates Oboe stream with new rate, and executes 128-sample cosine fade-in.

### M4 (Native Engine) ↔ M5 (Kotlin Framework / UI): JNI Contract
- Function Signatures:
  ```cpp
  // Lifecycle
  JNIEXPORT jboolean JNICALL Java_com_vban_receptor_native_NativeVbanEngine_startReceiver(
      JNIEnv* env, jobject thiz, jstring listenIp, jint port, jstring streamName, jint deviceId);
  JNIEXPORT void JNICALL Java_com_vban_receptor_native_NativeVbanEngine_stopReceiver(
      JNIEnv* env, jobject thiz);
  JNIEXPORT void JNICALL Java_com_vban_receptor_native_NativeVbanEngine_setAudioDevice(
      JNIEnv* env, jobject thiz, jint deviceId);

  // Telemetry Snapshot (Thread-safe, non-blocking)
  struct NativeTelemetry {
      uint32_t sampleRate;
      uint16_t channelCount;
      uint16_t bitDepth;
      uint64_t totalPacketsReceived;
      uint64_t lostPackets;
      float    jitterMs;
      float    bufferOccupancyPercent;
      float    peakLevelLeft;
      float    peakLevelRight;
  };
  JNIEXPORT jobject JNICALL Java_com_vban_receptor_native_NativeVbanEngine_getTelemetry(
      JNIEnv* env, jobject thiz);
  ```

### M5 (AudioDeviceManager) ↔ Android 14 Bit-Perfect HAL Contract
- Input: Selected `AudioDeviceInfo` (type `TYPE_USB_DEVICE` or `TYPE_USB_HEADSET`).
- Android 14+ API:
  - Query: `audioManager.getSupportedMixerAttributes(device)`
  - Find attribute where `attr.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT` and format matches incoming VBAN stream.
  - Set: `audioManager.setPreferredMixerAttributes(audioAttributes, device, bitPerfectAttr)`.
  - Pass `device.id` to native Oboe builder via `builder.setDeviceId(device.id)`.

---

## Code Layout

```
D:\Workspaces\vban-receptor\
├── app/
│   ├── build.gradle.kts
│   ├── src/
│   │   ├── main/
│   │   │   ├── AndroidManifest.xml
│   │   │   ├── cpp/
│   │   │   │   ├── CMakeLists.txt
│   │   │   │   ├── include/
│   │   │   │   │   ├── vban_protocol.hpp
│   │   │   │   │   ├── vban_parser.hpp
│   │   │   │   │   ├── vban_converter.hpp
│   │   │   │   │   ├── spsc_ring_buffer.hpp
│   │   │   │   │   ├── drift_compensator.hpp
│   │   │   │   │   ├── oboe_player.hpp
│   │   │   │   │   ├── vban_receiver.hpp
│   │   │   │   │   └── vban_engine.hpp
│   │   │   │   └── src/
│   │   │   │       ├── vban_parser.cpp
│   │   │   │       ├── vban_converter.cpp
│   │   │   │       ├── drift_compensator.cpp
│   │   │   │       ├── oboe_player.cpp
│   │   │   │       ├── vban_receiver.cpp
│   │   │   │       ├── vban_engine.cpp
│   │   │   │       └── jni_bridge.cpp
│   │   │   ├── java/com/vban/receptor/
│   │   │   │   ├── MainActivity.kt
│   │   │   │   ├── VbanApplication.kt
│   │   │   │   ├── audio/
│   │   │   │   │   ├── AudioDeviceManager.kt
│   │   │   │   │   └── BitPerfectController.kt
│   │   │   │   ├── service/
│   │   │   │   │   ├── VbanPlaybackService.kt
│   │   │   │   │   └── PowerNetworkLockManager.kt
│   │   │   │   ├── native/
│   │   │   │   │   └── NativeVbanEngine.kt
│   │   │   │   └── ui/
│   │   │   │       ├── DashboardScreen.kt
│   │   │   │       ├── VuMeter.kt
│   │   │   │       ├── BufferOccupancyGauge.kt
│   │   │   │       ├── DeviceSelector.kt
│   │   │   │       └── theme/
│   │   │   │           ├── Color.kt
│   │   │   │           ├── Theme.kt
│   │   │   │           └── Type.kt
├── native_tests/
│   ├── CMakeLists.txt
│   ├── test_main.cpp
│   ├── test_vban_protocol.cpp
│   ├── test_vban_converter.cpp
│   ├── test_spsc_buffer.cpp
│   └── test_drift_resampler.cpp
├── build.gradle.kts
├── settings.gradle.kts
├── gradle.properties
└── gradlew.bat
```
