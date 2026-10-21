# VBAN Receptor: Minimalist UI Redesign & Low-Latency Audio Stabilization

**Date:** 2026-10-21  
**Status:** Approved Specification  
**Target:** Android App (`app/src/main/java`) & C++ Audio Engine (`app/src/main/cpp`)  

---

## 1. Intent & Context

The user reported two key issues with the current VBAN Receptor Android application:
1. **User Interface Aesthetics:** The existing dashboard is rough/unpolished ("cutre"), relying on standard Material 3 form fields, cluttered cards, and lacking a modern, premium audiophile look.
2. **Audio Performance in Low-Latency Mode:** In `ULTRA_LOW` latency mode (5–10 ms), the audio stutters, drops out, or cracks severely over Wi-Fi ("se peta bastante el audio").

The objective is to redesign the UI around a refined **Minimalist Bauhaus / Teenage Engineering** aesthetic (OLED pure black `#000000`, bold typography, giant numerical latency readout, clean micro-metering) with a collapsible drawer for advanced tools, while stabilizing the native C++ audio engine in low-latency mode using adaptive jitter compensation, packet reordering, and graceful loss concealment.

---

## 2. Goals, Non-Goals & Anti-Goals

### Goals
- **Minimalist Bauhaus UI:**
  - Pure OLED black background (`#000000`) for maximum contrast and battery efficiency.
  - Large, bold numerical hero readout for real-time buffer latency (e.g., `7.2 ms`) with dynamic status indication (`● Óptimo · 0 drops / 0 glitches`).
  - Sleek horizontal dual-line peak stereo level meters (micro-bars) replacing cluttered gauge cards.
  - **Zero-Config One-Shot Auto-Connect:** On app startup, scan for VBAN streams; if exactly 1 stream is discovered on LAN within 1.5s, auto-connect immediately. If user manually presses "DETENER RECEPCIÓN", auto-connect remains paused to avoid looping.
  - **Collapsible Advanced Tools Panel:** Secondary expandable drawer containing the 64-band real-time Spectrum Analyzer and manual Latency Profile selector (`AUTO`, `ULTRA_LOW`, `BALANCED`, `ANTI_JITTER`).
  - Stream discovery drawer/card with one-tap connect when multiple streams exist, plus a clean collapsible manual IP/port option.
- **Low-Latency Audio Stabilization:**
  - Replace static clamping of buffer watermark in `ULTRA_LOW` with a dynamic jitter buffer based on RFC 3550 inter-arrival statistics.
  - Implement a lock-free UDP packet reordering queue (16 slots) in `VbanReceiver` to eliminate audio glitches caused by out-of-order packets.
  - Implement smooth single-packet loss concealment (PLC) with raised-cosine crossfade extrapolation rather than instant silence.
  - Maintain 100% C++ test and Android unit test pass rate.

### Non-Goals
- Adding complex equalizer DSP or audio effects.
- Supporting non-VBAN protocols (e.g., Dante, Ravenna, NDI).
- Supporting multi-stream mixing (VBAN Receptor remains a dedicated single active receiver per session).

### Anti-Goals
- Adding heavy UI or animation libraries (preserve the Ponytail lightweight footprint: zero unnecessary dependencies, pure Jetpack Compose).
- Increasing latency unnecessarily: the adaptive servo targets the absolute minimum glitch-free buffer (typically 8–12 ms on 5GHz Wi-Fi, 4–6 ms on Ethernet/USB).

---

## 3. UI/UX Specifications (Minimalist Bauhaus)

### Visual Language
- **Background:** `#000000` (OLED Pure Black).
- **Surface / Containers:** `#0A0A0A` and `#141414` with subtle `#262626` borders (1dp).
- **Typography:** Stark sans-serif, heavy weights (800/900) for headers and data, muted grays (`#737373`, `#A3A3A3`) for labels.
- **Accents:**
  - White (`#FFFFFF`) for primary data bars and values.
  - Green (`#34C759` / `#00E676`) for "LIVE" streaming indicator and optimal network state.
  - Red (`#FF3B30`) for the "DETENER RECEPCIÓN" button.

### Screen Layout Hierarchy
1. **Top Bar:**
   - Left: Bold brand `VBAN` + subtitle `FLOAT32 48K`.
   - Right: Minimal status pill (`LIVE` with green dot or `STANDBY` with gray dot).
   - Rightmost: Subtle audio device routing icon (USB DAC / Speaker / Bluetooth).
2. **Hero Latency & Buffer Readout:**
   - Large typography display: e.g. `7.2` in 48sp bold, with `ms` unit in 18sp.
   - Status caption: `● Óptimo · 0 drops / 0 glitches` (or `▲ Wi-Fi Jitter detectado` if jitter > 4.0 ms).
3. **Stereo Level Meters (Micro-bars):**
   - Two thin horizontal bars (6dp height) for Left and Right peak levels.
   - 60 FPS fast-attack exponential-decay rendering with numerical dBFS readout (`L -3.1 dB · R -3.5 dB`).
4. **Active Stream Card / Network Diagnostics:**
   - Active stream name (e.g., `Stream1`), IP/Port, sample rate, bit depth, and AAudio Exclusive status.
5. **Collapsible Advanced Drawer ("HERRAMIENTAS AVANZADAS"):**
   - Expandable via a subtle toggle at bottom or button.
   - Contains:
     - **64-Band Spectrum Analyzer:** High-contrast monochrome bars (white/gray) at 60 FPS.
     - **Profile Selector:** `AUTO (Adaptativo)` [Default], `Ultra-Low (5–10 ms)`, `Balanced (25–40 ms)`, `Anti-Jitter (100–200 ms)`.
6. **Connection & Discovery View (When not streaming or searching):**
   - **Auto-Connect Logic:**
     - On cold app launch in idle state: `VbanDiscoveryManager` scans for packets on port 6980.
     - If exactly 1 stream is discovered within 1.5 seconds, the app connects automatically.
     - If multiple streams are found, an elegant list of streams is presented with a bold `CONECTAR` button on each.
     - If 0 streams are found, the list shows "Buscando emisores..." with a clean toggle to enter IP and Port manually.
     - **Session Guard:** Pressing "DETENER RECEPCIÓN" disables auto-connect for that session to prevent re-connection loops.
     - **Socket Safety:** `VbanDiscoveryManager` pauses its UDP listening socket before `VbanReceiver` opens the native C++ audio socket, preventing port-sharing load-balancing packet loss.
7. **Main Action Button:**
   - Full-width button at bottom: "DETENER RECEPCIÓN" (red background) when running, or "CONECTAR" / "INICIAR" (white on black) when idle.

---

## 4. Native C++ Engine Specifications (Low-Latency Audio Fix)

### Architectural Decisions

#### 1. Dynamic Jitter Buffer Watermark (RFC 3550 Servo + Burst Guard)
- Calculate running 95th-percentile packet inter-arrival jitter $J$ in milliseconds.
- Minimum safe watermark target:
  $$\text{TargetLatency} = \max(\text{BurstDuration} \times 2, \; \text{BurstDuration} + 2.5 \times J)$$
- In `AUTO` and `ULTRA_LOW` modes:
  - If network is wired Ethernet / USB or pristine 5/6 GHz Wi-Fi ($J < 1\text{ ms}$), latency will dynamically lock to ~5–7 ms.
  - If Wi-Fi jitter rises ($J \approx 4\text{ ms}$), the target watermark smoothly expands to ~12–14 ms, preventing underruns completely.
- **Dual-Rate Servo & Burst Guard:**
  - Slew-limited PI servo ($\le 0.1\text{ PPM/s}$) maintains steady-state crystal drift inaudibly.
  - Sudden Wi-Fi burst step-delays that threaten starvation trigger a soft raised-cosine pre-fill or micro crossfade instead of dropping into AAudio starvation.

#### 2. Lock-Free UDP Packet Reordering Queue
- A lightweight 16-slot circular window buffer before writing into the SPSC ring buffer.
- Incoming packets with sequence number `nuFrame` are slotted by sequence index.
- If packets arrive slightly out of order over Wi-Fi, they are ordered in place before dequantization, completely eliminating digital glitch clicks.

#### 3. Single-Packet Loss Concealment (PLC)
- If a sequence gap of 1 packet is detected and the packet does not arrive within the reorder timeout:
  - Instead of leaving a hole in the SPSC ring buffer, extrapolate the last valid audio frame using a gentle raised-cosine decay crossfade.
  - When the subsequent packet arrives, smoothly crossfade in. This renders single packet drops virtually inaudible.

#### 4. Telemetry Semantics
- `drops`: Count of unrecoverable packet losses that exceeded the PLC window.
- `glitches`: Audio underruns recorded by AAudio (`underrunCount`).
- `jitterMs`: Real-time RFC 3550 inter-arrival jitter in milliseconds.

---

## 5. Verification Plan

1. **Native Engine Tests (CTest):**
   - Run `ctest` in `native_tests/build` verifying all tests pass, including new tests for packet reordering, jitter buffer calculations, and PLC.
2. **Android Unit Tests:**
   - Run `.\gradlew testDebugUnitTest` verifying `NativeVbanEngineTest`, `VbanDiscoveryManagerTest`, and ViewModel logic.
3. **Android Build Verification:**
   - Run `.\gradlew assembleDebug` to verify compilation for both `arm64-v8a` and `x86_64` targets.
