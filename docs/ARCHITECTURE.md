# VBAN Receptor for Android — Architecture & Technical Specifications

This document outlines the internal architecture, mathematical models, thread synchronization mechanisms, and audio pipeline design of **VBAN Receptor for Android**.

---

## 1. High-Level Architecture Overview

VBAN Receptor is built around a strictly decoupled, zero-allocation real-time audio pipeline engineered for high-resolution audio (up to 768 kHz, 32-bit Float, Bit-Perfect playback):

```
                          UDP Network (Port 6980)
                                     │
                                     ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Native Network Thread (vban_receiver)                                  │
│ - 4 MB SO_RCVBUF UDP Socket (Unicast & Multicast IP_ADD_MEMBERSHIP)    │
│ - 10-Step Header & Payload Packet Validation Pipeline                  │
│ - Monotonic nuFrame Tracking & RFC 3550 Jitter Estimation              │
│ - SIMD NEON / SSE Zero-Allocation Dequantization to Float32 [-1.0f, +1]│
└───────────────────────────────────┬────────────────────────────────────┘
                                     │ Interleaved Float32 Audio Frames
                                     ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Lock-Free SPSC Circular Buffer (spsc_ring_buffer)                      │
│ - Capacity: 2^18 frames (262,144 frames = 341.3 ms at 768 kHz)         │
│ - Cache-line aligned (alignas(64)) read & write atomic indices         │
│ - Monotonic uint64_t counters with acquire/release memory semantics    │
│ - ZERO dynamic allocations or system calls during streaming            │
└───────────────────────────────────┬────────────────────────────────────┘
                                     │ High-Priority Audio Callback Thread
                                     ▼
┌────────────────────────────────────────────────────────────────────────┐
│ Oboe Audio Engine & Clock Drift Compensator (oboe_player)              │
│ - Adaptive Clock Drift Servo: EMA Jitter Filter + PI Drift Controller  │
│ - Micro-Interpolation: Catmull-Rom Cubic Hermite Spline Resampling     │
│ - 64-Band FFT Spectrum Analyzer with Sub-Bass Interpolation (8Hz-22kHz)│
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
│ - PowerNetworkLockManager: WifiLock (LOW_LATENCY), WakeLock, Multicast │
│ - Android 14+ (API 34) Bit-Perfect USB DAC Bypass                      │
│   (AudioManager.setPreferredMixerAttributes MIXER_BEHAVIOR_BIT_PERFECT)│
│ - Jetpack Compose Reactive Dashboard (VU Meter, FFT Visualizer, UI)    │
│ - Zero-Config VBAN Network Discovery Manager                           │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Core Subsystems

### 2.1 VBAN Packet Validation & Dequantization (`vban_receiver`)
- **Protocol Conformance**: Validates the 28-byte VBAN audio header, checking:
  - 4-byte FourCC magic: `'V', 'B', 'A', 'N'` (`0x4E414256` Little-Endian).
  - Subprotocol: Audio (`0x00`).
  - Sample Rate Index: 21 official standard rates (6 kHz to 705.6 kHz) plus index 21 extension for 768 kHz.
  - Channels (`format_nbc + 1`): 1 to 8 channels.
  - Format / Codec: Linear PCM (`0x00`) and sub-formats (16-bit signed int, 24-bit packed signed int, 32-bit signed int, 32-bit IEEE float).
  - Stream Name: 16-byte null-padded stream name matching.
  - Expected Payload Length: strictly verifies packet payload matches $28 + (\text{nbs}+1)(\text{nbc}+1) \times \text{sampleSize}$.
- **SIMD Acceleration**: Dequantization routines support ARM NEON SIMD optimizations with portable scalar fallbacks.

### 2.2 Lock-Free SPSC Circular Buffer (`spsc_ring_buffer`)
- Single Producer (Network UDP Receiver Thread), Single Consumer (Oboe Audio Callback Thread).
- Fixed capacity: $2^{18}$ frames ($262,144$ frames), allowing fast bitwise wrapping `(index & MASK)` with zero modulo division.
- Cache-line separation (`alignas(64)`) between read index and write index to eliminate CPU core false sharing.
- Monotonic 64-bit sequence counters loaded and stored with `std::memory_order_acquire` and `std::memory_order_release`.
- Strictly no memory allocations, mutexes, locks, or system calls inside the real-time audio loop.

### 2.3 Adaptive Clock Drift Compensation (`drift_compensator`)
Because transmitter crystal oscillators (e.g. PC soundcard) and receptor oscillators (Android phone/DAC) will inherently diverge by several parts-per-million (PPM), an adaptive servo mechanism ensures the buffer level remains stable:
- **Jitter Filter**: Second-order slow Exponential Moving Average (EMA) tracks the true mean buffer watermark over time, filtering network jitter.
- **PI Controller**: Proportional-Integral controller modulates the resampling ratio within a bounded range ($\pm 100$ PPM) with max slew rate $\le 0.1\text{ PPM/s}$ to guarantee pitch changes are below human auditory detection thresholds.
- **Catmull-Rom Cubic Hermite Spline Resampler**: 4-point micro-interpolation produces smooth, glitch-free continuous audio without the clicks or phase distortion of sample-dropping or linear interpolation.

### 2.4 FFT Spectrum Analyzer (`fft_analyzer`)
- Radix-2 Cooley-Tukey FFT with precomputed Hann windowing and bit-reversal lookup tables.
- Calibrated logarithmic frequency bins from 8 Hz (sub-bass) to 22 kHz (high treble).
- Continuous linear interpolation for narrow sub-bass bins to provide smooth visual dynamics on high-refresh screens (60–120 FPS).

### 2.5 Android 14+ Bit-Perfect Audio Routing (`BitPerfectController`)
- Leverages Android 14 (API 34) `AudioManager.setPreferredMixerAttributes` with `MIXER_BEHAVIOR_BIT_PERFECT`.
- When an external audiophile USB DAC is connected, Android's internal audio resampler and mixer are completely bypassed, enabling pristine bit-perfect digital audio transmission straight to the DAC hardware.
- Gracefully falls back to AAudio exclusive low-latency mode on Android 8.0 to Android 13 devices.

### 2.6 Zero-Config VBAN Network Discovery (`VbanDiscoveryManager`)
- Listens passively on UDP port 6980 for incoming VBAN streams.
- Parses packet headers, extracts stream names, IP addresses, sample rates, channel configurations, and bit depths.
- Automatically exposes discovered transmitters in the Jetpack Compose UI for one-tap selection.
- Evicts inactive streams dynamically after configurable timeout periods.

---

## 3. Real-Time Thread Topology & Priority Guarantees

| Thread Name | Owner / Origin | Scheduling / Priority | Real-Time Constraints |
|:------------|:---------------|:----------------------|:----------------------|
| **Oboe Audio Callback** | AAudio / Oboe HAL | Real-Time Audio Priority (SCHED_FIFO where available) | Zero syscalls, zero mutexes, zero allocations. Reads directly from SPSC ring buffer. |
| **Native VBAN Receiver** | Native pthread | High Priority Native Thread | Blocks on `recvfrom` with 4 MB socket buffer; writes directly to SPSC ring buffer. |
| **Android Service Thread** | Kotlin / Coroutines | Normal Priority Background Worker | Telemetry polling (60 Hz), power lock management, notification updates. |
| **UI Main Thread** | Android Jetpack Compose | UI Main Thread (60–120 Hz) | Non-blocking telemetry consumption via Kotlin `StateFlow`. |

---

## 4. Verification & Testing Framework

The codebase contains an exhaustive, multi-tier test infrastructure:
- **C++ Native Unit Tests (`native_tests/`)**:
  - Comprehensive coverage of VBAN header validation, 21 sample rate decodings, Catmull-Rom micro-resampler, SPSC concurrency under multi-threaded stress, and FFT analysis.
- **Python End-to-End Test Suite (`e2e_tests/`)**:
  - 84 automated test cases covering protocol compliance, boundary conditions, clock drift simulation, adversarial network packets, and system lifecycle behavior across 4 tiers.
- **Android Unit Tests (`app/src/test/`)**:
  - Tests for network discovery, packet parsing, stream caching, and timeout eviction logic.
