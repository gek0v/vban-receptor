# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.0.0] - 2026-10-19

### Added
- **Native Audio & Network Core (C++20 & NDK 26.1)**:
  - Real-time UDP network receiver bound to port 6980 with 4 MB socket receive buffer and multicast group support.
  - 10-step RFC-compliant VBAN packet parser and validator.
  - Full support for all 21 official VBAN sample rates ($6\text{ kHz}$ to $705.6\text{ kHz}$) plus the $768\text{ kHz}$ audiophile extension.
  - Zero-allocation audio dequantizers for 16-bit PCM, 24-bit packed LE PCM, 32-bit int PCM, and 32-bit IEEE Float with SIMD NEON optimizations.
  - Lock-free Single-Producer Single-Consumer (SPSC) circular buffer ($262,144$ frames capacity, cache-line aligned with atomic acquire/release memory semantics).
  - Proportional-Integral (PI) clock drift compensator with 2nd-order slow EMA jitter filtering.
  - Glitch-free 4-point Catmull-Rom cubic Hermite spline micro-resampler.
  - Oboe 1.9.0 AAudio exclusive mode low-latency audio player.
  - Decoupled stream format reconfiguration with 128-sample cosine fade-out/fade-in.
- **Android Framework & System Integration**:
  - Android 14+ (API 34) Bit-Perfect USB DAC bypass via `AudioManager.setPreferredMixerAttributes` and `MIXER_BEHAVIOR_BIT_PERFECT`.
  - Android 13 and below AAudio exclusive mode fallback.
  - Android 14 compliant Foreground Service (`type="mediaPlayback"`) with persistent media notification.
  - Triple power-network lock manager acquiring `WifiLock` (`WIFI_MODE_FULL_LOW_LATENCY`), `WakeLock`, and `MulticastLock` for uninterrupted playback with screen off.
  - Audio device enumeration with dynamic USB DAC hotplug callbacks.
  - Zero-config VBAN network stream auto-discovery with packet parsing and timeout eviction.
- **UI & Telemetry (Jetpack Compose Material 3)**:
  - Modern reactive dashboard with connection controls, stream name, port, and audio output device selector.
  - Real-time 60 FPS dual stereo VU meters calibrated from $-60\text{ dBFS}$ to $0\text{ dBFS}$ with peak-hold decay.
  - 64-band logarithmic FFT Spectrum Analyzer calibrated from $8\text{ Hz}$ (sub-bass) to $22\text{ kHz}$ (treble) with sub-bass linear interpolation.
  - Real-time buffer fill level gauge, RFC 3550 network jitter meter, and packet loss counters.
- **Testing & Verification**:
  - Standalone C++ test runner with CTest integration.
  - Comprehensive 84-case automated Python End-to-End verification suite across 4 tiers (Feature Coverage, Boundaries & Corners, Cross-Feature Combinations, Real-World Scenarios).
  - Android JVM unit tests for network discovery and packet ingestion.
- **Open Source Preparation**:
  - MIT License with VBAN® protocol trademark notice.
  - CI/CD workflow with GitHub Actions.
  - Architecture documentation, contribution guidelines, and issue templates.
