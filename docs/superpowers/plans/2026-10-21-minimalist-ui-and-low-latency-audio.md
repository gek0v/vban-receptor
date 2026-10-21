# Minimalist UI Redesign & Low-Latency Audio Stabilization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform the VBAN Receptor UI into a sleek Bauhaus Minimalist aesthetic (OLED pure black, hero latency dial, micro-bars, zero-config auto-connect) and stabilize the C++ native engine for glitch-free low-latency streaming over Wi-Fi.

**Architecture:** Extend C++ `vban_receiver` with a 16-slot lock-free UDP packet reordering queue and single-packet loss concealment (PLC), update `drift_compensator` with an adaptive jitter buffer sizing model (RFC 3550) and underrun burst-guard, extend JNI telemetry with latency, drop and glitch metrics, and overhaul Jetpack Compose components (`DashboardScreen`, `LatencyHeroDial`, `MinimalStereoMeter`, `CollapsibleDrawer`) with a one-shot auto-connect state machine.

**Tech Stack:** C++20 (CMake, Oboe, SPSC lock-free ring buffer, RFC 3550 EWMA), Android NDK, Kotlin 2.1+, Jetpack Compose, Material 3, Coroutines/StateFlow.

**Spec:** `docs/superpowers/specs/2026-10-21-ui-redesign-and-low-latency-audio-design.md`

## Global Constraints
- Target latency in AUTO/Ultra-Low mode must dynamically adapt between 5 ms (clean LAN) and 15 ms (Wi-Fi jitter) based on RFC 3550 $J_{95}$.
- Slew rate limiting for steady-state clock drift must remain $\le 0.1\text{ PPM/s}$ to guarantee pitch inaudibility.
- Auto-connect must be strictly one-shot on cold start; pressing "DETENER RECEPCIÓN" must latch auto-connect to paused for the remainder of the session.
- Discovery socket must be paused/closed before native audio socket binds to avoid Linux kernel SO_REUSEPORT load-balancing loss on UDP 6980.
- Zero new third-party dependencies: preserve lightweight Ponytail footprint using native platform and standard Jetpack libraries.
- All native CTests and Gradle unit tests must pass with 100% success rate.

## Review Focus
- Rapid out-of-order packet bursts over Wi-Fi: verify 16-slot reorder queue delivers packets monotonically ordered without stalls (pinned to Task 1).
- Packet loss gap > 1: verify PLC extrapolates exactly 1 packet, then smoothly drops to silence without assertion/overflow (pinned to Task 1).
- Wi-Fi jitter spike exceeding 10 ms: verify dynamic watermark expands and burst guard prevents resampler unpriming (pinned to Task 2).
- Manual disconnect while single stream is still transmitting on LAN: verify app does not immediately re-auto-connect (pinned to Task 6).
- Discovery and playback switching: verify no socket descriptor leak or bind collision on UDP port 6980 (pinned to Task 6).

---

### Task 1: UDP Packet Reordering Queue & Packet Loss Concealment (C++)

**Files:**
- Create: `app/src/main/cpp/include/packet_reorder_queue.hpp`
- Create: `native_tests/test_packet_reorder.cpp`
- Modify: `native_tests/CMakeLists.txt`
- Modify: `app/src/main/cpp/include/vban_receiver.hpp`
- Modify: `app/src/main/cpp/src/vban_receiver.cpp`

**Interfaces:**
- Produces: `vban::PacketReorderQueue` class:
  - `bool push(uint32_t nuFrame, const float* samples, size_t sampleCount)`
  - `size_t popOrdered(float* outSamples, size_t maxSamples, bool& wasPlc)`
  - `void reset()`
- Consumes: `SpscRingBuffer`, `vban_protocol.hpp`.

- [ ] **Step 1: Write the failing native test for `PacketReorderQueue`**

In `native_tests/test_packet_reorder.cpp`:
```cpp
#include "test_framework.hpp"
#include "packet_reorder_queue.hpp"

TEST_CASE("PacketReorderQueue reassembles out-of-order sequence (1, 3, 2, 4)", "[reorder]") {
    vban::PacketReorderQueue<float, 16, 256 * 2> queue;
    float p1[4] = {1.f, 1.f, 1.f, 1.f};
    float p2[4] = {2.f, 2.f, 2.f, 2.f};
    float p3[4] = {3.f, 3.f, 3.f, 3.f};
    float p4[4] = {4.f, 4.f, 4.f, 4.f};

    queue.push(101, p1, 4);
    queue.push(103, p3, 4);
    queue.push(102, p2, 4);
    queue.push(104, p4, 4);

    float out[16] = {};
    bool plc = false;
    size_t popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 16);
    ASSERT_EQ(out[0], 1.f);
    ASSERT_EQ(out[4], 2.f);
    ASSERT_EQ(out[8], 3.f);
    ASSERT_EQ(out[12], 4.f);
}
```

- [ ] **Step 2: Add test to `native_tests/CMakeLists.txt` and verify it fails**

Register `test_packet_reorder.cpp` in `native_tests/CMakeLists.txt`.
Run: `cmake --build native_tests/build --target test_vban; cd native_tests/build; ctest -R test_packet_reorder --output-on-failure`
Expected: FAIL (header `packet_reorder_queue.hpp` does not exist).

- [ ] **Step 3: Implement `PacketReorderQueue` in `app/src/main/cpp/include/packet_reorder_queue.hpp`**

Implement fixed-size 16-slot ring queue:
- Indexed by `nuFrame % 16`.
- Tracks `expectedNuFrame`.
- If packet gap == 1 and next packet not received within timeout/slots, inject raised-cosine crossfade extrapolation from previous packet tail (PLC).
- Integrate into `VbanReceiver::processPacket` before `m_ringBuffer.write`.

- [ ] **Step 4: Verify native tests pass**

Run: `cmake --build native_tests/build --target test_vban; cd native_tests/build; ctest -R test_packet_reorder --output-on-failure`
Expected: PASS (100% tests pass).

- [ ] **Step 5: Commit**

```bash
git add app/src/main/cpp/include/packet_reorder_queue.hpp app/src/main/cpp/include/vban_receiver.hpp app/src/main/cpp/src/vban_receiver.cpp native_tests/
git commit -m "feat(audio): add lock-free UDP packet reorder queue and PLC"
```

---

### Task 2: Dynamic Jitter Watermark & Underrun Burst Guard (C++)

**Files:**
- Modify: `app/src/main/cpp/include/drift_compensator.hpp`
- Modify: `app/src/main/cpp/src/drift_compensator.cpp`
- Modify: `app/src/main/cpp/src/vban_engine.cpp`
- Modify: `native_tests/test_drift_resampler.cpp`

**Interfaces:**
- Modifies: `ClockDriftCompensator`:
  - `void setNetworkJitter(float jitterMs, uint32_t burstFrames)`
  - `[[nodiscard]] float getTargetWatermarkMs() const noexcept`
  - `[[nodiscard]] uint64_t getGlitchCount() const noexcept`
  - `[[nodiscard]] uint64_t getUnrecoverableDrops() const noexcept`

- [ ] **Step 1: Write the failing test for dynamic watermark in `test_drift_resampler.cpp`**

```cpp
TEST_CASE("Dynamic Watermark adapts to high network jitter", "[drift]") {
    vban::ClockDriftConfig config;
    config.ringBufferCapacity = 262144;
    vban::ClockDriftCompensator compensator(config);
    compensator.reconfigure(48000, 2);

    // Baseline jitter 0.5 ms -> target latency ~6-8 ms
    compensator.setNetworkJitter(0.5f, 192);
    float lowTarget = compensator.getTargetWatermarkMs();
    ASSERT_TRUE(lowTarget >= 5.0f && lowTarget <= 10.0f);

    // High jitter 6.0 ms -> target latency expands to ~12-16 ms
    compensator.setNetworkJitter(6.0f, 192);
    float highTarget = compensator.getTargetWatermarkMs();
    ASSERT_TRUE(highTarget > lowTarget);
    ASSERT_TRUE(highTarget >= 12.0f);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build native_tests/build --target test_vban; cd native_tests/build; ctest -R test_drift_resampler --output-on-failure`
Expected: FAIL (`setNetworkJitter` or `getTargetWatermarkMs` not defined).

- [ ] **Step 3: Implement Dynamic Jitter Watermark & Burst Guard**

In `drift_compensator.cpp`:
- Implement dynamic target calculation:
  `targetMs = std::max(burstMs * 2.0f, burstMs + 2.5f * jitterMs);`
  Clamped between 5.0 ms and 25.0 ms for low-latency mode.
- Update `ClockDriftCompensator::process` to count `m_glitchCount` when starvation occurs, and execute soft cosine crossfade to prevent pops.

- [ ] **Step 4: Verify native tests pass**

Run: `cmake --build native_tests/build --target test_vban; cd native_tests/build; ctest -R test_drift_resampler --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/cpp/include/drift_compensator.hpp app/src/main/cpp/src/drift_compensator.cpp app/src/main/cpp/src/vban_engine.cpp native_tests/test_drift_resampler.cpp
git commit -m "feat(audio): add dynamic jitter watermark servo and burst guard"
```

---

### Task 3: JNI Bridge & Native Telemetry Extensions

**Files:**
- Modify: `app/src/main/cpp/src/jni_bridge.cpp`
- Modify: `app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt`
- Create: `app/src/test/java/com/vban/receptor/native/NativeTelemetryTest.kt`

**Interfaces:**
- Produces: `NativeTelemetry` fields:
  - `val targetWatermarkMs: Float`
  - `val unrecoverableDrops: Long`
  - `val glitchCount: Long`

- [ ] **Step 1: Write failing Kotlin test for extended telemetry**

In `app/src/test/java/com/vban/receptor/native/NativeTelemetryTest.kt`:
```kotlin
package com.vban.receptor.native

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class NativeTelemetryTest {
    @Test
    fun testTelemetryDefaultValues() {
        val telemetry = NativeTelemetry(
            sampleRate = 48000,
            jitterMs = 2.5f,
            targetWatermarkMs = 8.0f,
            unrecoverableDrops = 0L,
            glitchCount = 0L
        )
        assertEquals(8.0f, telemetry.targetWatermarkMs, 0.01f)
        assertEquals(0L, telemetry.unrecoverableDrops)
        assertEquals(0L, telemetry.glitchCount)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.\gradlew testDebugUnitTest --tests com.vban.receptor.native.NativeTelemetryTest`
Expected: FAIL (unresolved reference `targetWatermarkMs`).

- [ ] **Step 3: Update `NativeVbanEngine.kt` and `jni_bridge.cpp`**

In `NativeVbanEngine.kt`:
- Add `targetWatermarkMs: Float = 0.0f`, `unrecoverableDrops: Long = 0L`, `glitchCount: Long = 0L` to `data class NativeTelemetry`.
In `jni_bridge.cpp`:
- Map native engine metrics into the Java `NativeTelemetry` constructor.

- [ ] **Step 4: Verify Kotlin tests pass**

Run: `.\gradlew testDebugUnitTest --tests com.vban.receptor.native.NativeTelemetryTest`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/cpp/src/jni_bridge.cpp app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt app/src/test/java/com/vban/receptor/native/NativeTelemetryTest.kt
git commit -m "feat(jni): expose dynamic watermark, drop and glitch metrics in NativeTelemetry"
```

---

### Task 4: Bauhaus Minimalist Theme & Palette

**Files:**
- Modify: `app/src/main/java/com/vban/receptor/ui/theme/Color.kt`
- Modify: `app/src/main/java/com/vban/receptor/ui/theme/Theme.kt`
- Modify: `app/src/main/java/com/vban/receptor/ui/theme/Type.kt`

**Interfaces:**
- Produces:
  - `OledBlack = Color(0xFF000000)`
  - `DarkSurface = Color(0xFF0A0A0A)`
  - `DarkBorder = Color(0xFF262626)`
  - `LiveGreen = Color(0xFF34C759)`
  - `StopRed = Color(0xFFFF3B30)`
  - `TextMuted = Color(0xFF737373)`

- [ ] **Step 1: Write test for Theme colors**

Create `app/src/test/java/com/vban/receptor/ui/ThemeTest.kt`:
```kotlin
package com.vban.receptor.ui

import androidx.compose.ui.graphics.Color
import com.vban.receptor.ui.theme.OledBlack
import org.junit.Assert.assertEquals
import org.junit.Test

class ThemeTest {
    @Test
    fun testOledBlackValue() {
        assertEquals(0xFF000000.toInt(), OledBlack.value.toInt())
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.\gradlew testDebugUnitTest --tests com.vban.receptor.ui.ThemeTest`
Expected: FAIL (unresolved reference `OledBlack`).

- [ ] **Step 3: Update `Color.kt`, `Theme.kt`, and `Type.kt`**

Define OLED black palette, stark Bauhaus typography styles, and dark Material3 color schemes.

- [ ] **Step 4: Run test to verify it passes**

Run: `.\gradlew testDebugUnitTest --tests com.vban.receptor.ui.ThemeTest`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/java/com/vban/receptor/ui/theme/ app/src/test/java/com/vban/receptor/ui/ThemeTest.kt
git commit -m "feat(ui): add OLED Bauhaus minimalist theme and color palette"
```

---

### Task 5: Minimalist UI Components (`LatencyHeroDial`, `MinimalStereoMeter`, `CollapsibleDrawer`)

**Files:**
- Create: `app/src/main/java/com/vban/receptor/ui/components/LatencyHeroDial.kt`
- Create: `app/src/main/java/com/vban/receptor/ui/components/MinimalStereoMeter.kt`
- Create: `app/src/main/java/com/vban/receptor/ui/components/CollapsibleDrawer.kt`

**Interfaces:**
- `LatencyHeroDial(latencyMs: Float, jitterMs: Float, drops: Long, glitches: Long, isRunning: Boolean, modifier: Modifier)`
- `MinimalStereoMeter(leftDbfs: Float, rightDbfs: Float, isRunning: Boolean, modifier: Modifier)`
- `CollapsibleDrawer(isExpanded: Boolean, onToggle: () -> Unit, content: @Composable () -> Unit)`

- [ ] **Step 1: Implement `LatencyHeroDial.kt`**

Renders bold 48sp numerical latency with `ms` unit, status dot, and health caption (`● Óptimo` or `▲ Jitter`).

- [ ] **Step 2: Implement `MinimalStereoMeter.kt`**

Renders 2 horizontal thin 6dp micro-bars with smooth decay and dBFS text readouts.

- [ ] **Step 3: Implement `CollapsibleDrawer.kt`**

Renders expandable bottom drawer for advanced tools (Spectrum Analyzer + Profile Selector).

- [ ] **Step 4: Verify compilation via gradle unit tests**

Run: `.\gradlew compileDebugKotlin`
Expected: BUILD SUCCESSFUL.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/java/com/vban/receptor/ui/components/
git commit -m "feat(ui): add LatencyHeroDial, MinimalStereoMeter, and CollapsibleDrawer components"
```

---

### Task 6: One-Shot Auto-Connect State Machine & Socket Safety

**Files:**
- Modify: `app/src/main/java/com/vban/receptor/network/VbanDiscoveryManager.kt`
- Modify: `app/src/main/java/com/vban/receptor/ui/DashboardScreen.kt`
- Modify: `app/src/test/java/com/vban/receptor/network/VbanDiscoveryTest.kt`

**Interfaces:**
- `VbanDiscoveryManager`:
  - `fun pauseDiscoverySocket()`
  - `fun resumeDiscoverySocket()`
- Auto-connect state machine:
  - One-shot flag `hasAutoConnectedThisSession: Boolean`
  - Explicit manual stop flag `userRequestedStop: Boolean`

- [ ] **Step 1: Write test for Auto-Connect and Socket Pause in `VbanDiscoveryTest.kt`**

```kotlin
@Test
fun testDiscoverySocketPauseAndResume() = runTest {
    val manager = VbanDiscoveryManager()
    manager.start()
    assertTrue(manager.isScanning.value)
    manager.pauseDiscoverySocket()
    assertFalse(manager.isScanning.value)
    manager.resumeDiscoverySocket()
    assertTrue(manager.isScanning.value)
    manager.stop()
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `.\gradlew testDebugUnitTest --tests com.vban.receptor.network.VbanDiscoveryTest.testDiscoverySocketPauseAndResume`
Expected: FAIL (`pauseDiscoverySocket` not defined).

- [ ] **Step 3: Implement `pauseDiscoverySocket()` and `resumeDiscoverySocket()`**

In `VbanDiscoveryManager.kt`:
- Close multicast socket on pause to release UDP port 6980 for the native engine.
- Re-open when resuming.

- [ ] **Step 4: Verify test passes**

Run: `.\gradlew testDebugUnitTest --tests com.vban.receptor.network.VbanDiscoveryTest.testDiscoverySocketPauseAndResume`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/java/com/vban/receptor/network/VbanDiscoveryManager.kt app/src/test/java/com/vban/receptor/network/VbanDiscoveryTest.kt
git commit -m "feat(discovery): add socket pause/resume and one-shot auto-connect safety"
```

---

### Task 7: Full Dashboard Screen Overhaul & End-to-End Verification

**Files:**
- Modify: `app/src/main/java/com/vban/receptor/ui/DashboardScreen.kt`

**Interfaces:**
- Assembles:
  - Top Bar with `VBAN FLOAT32` and `LIVE` / `STANDBY` pill.
  - `LatencyHeroDial` displaying real-time milliseconds.
  - `MinimalStereoMeter` micro-bars.
  - Compact active stream info card.
  - Discovered streams list with one-tap connect & one-shot auto-connect.
  - `CollapsibleDrawer` housing `SpectrumAnalyzer` and `LatencyProfileSelector`.
  - Full-width Start / Stop Action button.

- [ ] **Step 1: Overhaul `DashboardScreen.kt` with Bauhaus Minimalist Layout**

Replace old cluttered form cards with the new Bauhaus components and state machine.

- [ ] **Step 2: Run CTest native test suite**

Run: `cmake --build native_tests/build --target test_vban; cd native_tests/build; ctest --output-on-failure`
Expected: 100% tests passing.

- [ ] **Step 3: Run Android unit tests**

Run: `.\gradlew testDebugUnitTest`
Expected: 100% unit tests passing.

- [ ] **Step 4: Run Android debug build verification**

Run: `.\gradlew assembleDebug`
Expected: BUILD SUCCESSFUL (APKs built for arm64-v8a and x86_64).

- [ ] **Step 5: Commit**

```bash
git add app/src/main/java/com/vban/receptor/ui/DashboardScreen.kt
git commit -m "feat(ui): complete Bauhaus minimalist dashboard overhaul with auto-connect"
```
