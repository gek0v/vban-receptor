# Survey & Technical Remediation Report: Milestone 3 Starvation Recovery & Resampler Re-Priming

**Author**: Explorer Agent (`explorer_m3_fix`)  
**Date**: 2026-10-18T15:52:00Z  
**Target Milestone**: Milestone 3 Iteration 2 (Remediation)  
**Parent Orchestrator ID**: `9bb0c473-fb0a-499a-bbf2-a430786d0058`  
**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m3_fix`  

---

## 1. Executive Summary

During adversarial review of Milestone 3 (`reviewer_m3_1`), a critical resilience defect was identified in `ClockDriftCompensator` / `FadeEngine`:
- **Major Finding 1**: When an SPSC ring buffer underrun occurs (`framesProduced < outputFrames`), `m_fadeEngine.emergencyFadeOut` sets state to `FadeState::Muted`. When healthy frames resume and `framesProduced == outputFrames`, `m_fadeEngine.process` zeroes the output buffer via `std::memset` because `m_state` remains `FadeState::Muted` indefinitely. No subsequent audio is ever output until the stream is externally destroyed and recreated.
- **Minor Finding 2**: During complete starvation, `m_isPrimed` remains `true` in `CatmullRomResampler`. When playback resumes after hundreds of dropped packets, the resampler interpolates between stale history samples from before the drop and the new incoming stream.

This report provides the complete mathematical, algorithmic, and architectural remediation design, including exact C++20 diffs, full code templates, and a multi-phase unit test validating starvation recovery, automatic fade-in unlatching, and resampler re-priming.

---

## 2. Root Cause Analysis

### 2.1 Major Finding 1: Permanent Mute Latch on Underrun Recovery

#### Code Inspection
In `app/src/main/cpp/include/drift_compensator.hpp`, lines 622–626:
```cpp
if (framesProduced < outputFrames) [[unlikely]] {
    m_fadeEngine.emergencyFadeOut(output, framesProduced, outputFrames, channels);
} else {
    m_fadeEngine.process(output, outputFrames, channels);
}
```
And in `app/src/main/cpp/src/drift_compensator.cpp`, lines 34–37:
```cpp
if (m_state == FadeState::Muted) {
    std::memset(buffer, 0, frames * channels * sizeof(float));
    return;
}
```

#### Failure Dynamics
1. Network packet jitter or momentary burst starvation causes the ring buffer to deliver fewer frames than requested (`framesProduced < outputFrames`).
2. `emergencyFadeOut` smoothly attenuates whatever partial frames were produced, zeroes remaining frames, and sets `m_state = FadeState::Muted; m_sampleIndex = FADE_WINDOW_SIZE;`.
3. Packets arrive over UDP, replenishing the SPSC ring buffer.
4. On the next audio callback (e.g. `OboePlayer::onAudioReady`), `m_resampler.process` successfully produces `outputFrames` samples (`framesProduced == outputFrames`).
5. Execution branches to `else { m_fadeEngine.process(output, outputFrames, channels); }`.
6. Inside `FadeEngine::process`, `m_state` is evaluated. Because `m_state == FadeState::Muted`, `FadeEngine::process` zeroes out the newly produced valid audio buffer with `std::memset` and returns immediately.
7. `m_state` remains `FadeState::Muted`. Neither `FadeEngine::process` nor `ClockDriftCompensator::process` ever invokes `startFadeIn()`.
8. Audio output is permanently silenced for all subsequent callbacks.

#### Algorithmic Resolution
When `framesProduced == outputFrames`, check if `m_fadeEngine.isMuted()`. If so, invoke `m_fadeEngine.startFadeIn()` before calling `m_fadeEngine.process(output, outputFrames, channels)`.
- If `isMuted()` is true, `startFadeIn()` resets `m_sampleIndex = 0` and sets `m_state = FadeState::FadingIn`.
- `FadeEngine::process` multiplies audio by the 128-sample raised cosine window $w(n) = 0.5 \cdot (1.0 - \cos(\pi n / 128))$, transitioning cleanly to `FadeState::Active` once $n \ge 128$.
- If `outputFrames < 128` (e.g. 64 frames), callback 1 fades in frames 0..63 (`m_state` remains `FadingIn`). In callback 2, `isMuted()` is false, so `startFadeIn()` is not re-triggered, and frames 64..127 complete the fade-in, transitioning to `FadeState::Active`.

---

### 2.2 Minor Finding 2: CatmullRomResampler Stale History Retention Across Dropouts

#### Code Inspection
In `app/src/main/cpp/include/drift_compensator.hpp`, lines 291–298 and 312–319:
```cpp
while (m_phase >= 1.0) {
    m_phase -= 1.0;
    if (!advanceOneInputFrame(ringBuffer, channels)) {
        m_underrunFrames += (outputFrames - outFrame);
        m_totalOutputFramesGenerated += outFrame;
        return outFrame;
    }
}
```
And member variables (lines 457–468):
```cpp
alignas(64) float m_history[4][MAX_CHANNELS]{};
double m_phase{0.0};
bool m_isPrimed{false};
```

#### Failure Dynamics
1. When `advanceOneInputFrame` fails due to ring buffer depletion, `m_isPrimed` remains `true`.
2. `m_history` retains the 4 input samples from the end of the previous burst.
3. If thousands of frames elapse during network silence, the stream is resumed with completely new audio packets.
4. If `m_isPrimed` remains `true`, the first interpolated samples combine the stale pre-dropout samples with the newly arrived post-dropout samples, potentially introducing a high-frequency spline edge or numerical discontinuity.

#### Algorithmic Resolution
1. Introduce a dedicated public method `void unprime() noexcept` in `CatmullRomResampler` that clears `m_history`, resets `m_phase = 0.0`, clears `m_stagingAvailable = 0; m_stagingReadPos = 0;`, and sets `m_isPrimed = false;` without resetting cumulative counters (`m_totalInputFramesConsumed`, etc.).
2. In `CatmullRomResampler::process`, invoke `unprime()` whenever `advanceOneInputFrame` returns `false`.
3. In `ClockDriftCompensator::process`, invoke `m_resampler.unprime()` whenever `framesProduced < outputFrames`.
4. When fresh audio arrives, `CatmullRomResampler::process` sees `!m_isPrimed`, invokes `primeFromRingBuffer(ringBuffer, channels)`, and reads up to 4 contiguous frames from the head of the new audio stream, cleanly resetting phase to $0.0$ and re-priming $p_0, p_1, p_2, p_3$.

---

## 3. Exact Code Modifications

### 3.1 `app/src/main/cpp/include/drift_compensator.hpp`

#### Modification A: Refactor `reset()` and add `unprime()` to `CatmullRomResampler`
Lines 208–221:
```cpp
    void reset() noexcept {
        unprime();
        m_totalInputFramesConsumed = 0;
        m_totalOutputFramesGenerated = 0;
        m_underrunFrames = 0;
    }

    void unprime() noexcept {
        for (size_t i = 0; i < 4; ++i) {
            for (size_t ch = 0; ch < MAX_CHANNELS; ++ch) {
                m_history[i][ch] = 0.0f;
            }
        }
        m_phase = 0.0;
        m_isPrimed = false;
        m_stagingAvailable = 0;
        m_stagingReadPos = 0;
    }
```

#### Modification B: Unprime resampler on underrun in `CatmullRomResampler::process`
Lines 293–298 (stereo loop) and lines 314–319 (multichannel loop):
```cpp
                while (m_phase >= 1.0) {
                    m_phase -= 1.0;
                    if (!advanceOneInputFrame(ringBuffer, channels)) {
                        m_underrunFrames += (outputFrames - outFrame);
                        m_totalOutputFramesGenerated += outFrame;
                        unprime();
                        return outFrame;
                    }
                }
```

#### Modification C: Auto un-mute and resampler unprime in `ClockDriftCompensator::process`
Lines 622–627:
```cpp
        if (framesProduced < outputFrames) [[unlikely]] {
            m_resampler.unprime();
            m_fadeEngine.emergencyFadeOut(output, framesProduced, outputFrames, channels);
        } else {
            if (m_fadeEngine.isMuted()) [[unlikely]] {
                m_fadeEngine.startFadeIn();
            }
            m_fadeEngine.process(output, outputFrames, channels);
        }
```

---

### 3.2 `app/src/main/cpp/src/drift_compensator.cpp`

Inspection of `drift_compensator.cpp` confirms:
- `ClockDriftCompensator::process` is a template method fully defined in `drift_compensator.hpp`.
- `CatmullRomResampler::unprime` is defined inline in `drift_compensator.hpp`.
- `FadeEngine::emergencyFadeOut` already sets `m_state = FadeState::Muted`.
- Therefore, **no changes are needed in `drift_compensator.cpp`**.

---

### 3.3 `native_tests/test_drift_resampler.cpp`

Add Test Case 11: `drift_resampler_starvation_recovery_unmutes_and_resumes`.
This test rigorously exercises:
1. Active playback before underrun.
2. Complete buffer starvation leading to `emergencyFadeOut`, `FadeState::Muted`, and `!resampler.isPrimed()`.
3. Repeated calls during ongoing starvation maintaining silence and unprimed state.
4. Refill of SPSC ring buffer triggering automatic un-mute (`startFadeIn`), re-priming from contiguous frames, and smooth cosine fade-in.
5. Multi-block partial buffer fade-in (e.g. 64 frames per block) transitioning from `FadingIn` to `Active` without premature re-triggering.

---

## 4. Complete Unified Diff Specification

```diff
--- a/app/src/main/cpp/include/drift_compensator.hpp
+++ b/app/src/main/cpp/include/drift_compensator.hpp
@@ -208,6 +208,18 @@ public:
     void reset() noexcept {
+        unprime();
+        m_totalInputFramesConsumed = 0;
+        m_totalOutputFramesGenerated = 0;
+        m_underrunFrames = 0;
+    }
+
+    void unprime() noexcept {
         for (size_t i = 0; i < 4; ++i) {
             for (size_t ch = 0; ch < MAX_CHANNELS; ++ch) {
                 m_history[i][ch] = 0.0f;
             }
         }
         m_phase = 0.0;
         m_isPrimed = false;
         m_stagingAvailable = 0;
         m_stagingReadPos = 0;
-        m_totalInputFramesConsumed = 0;
-        m_totalOutputFramesGenerated = 0;
-        m_underrunFrames = 0;
     }
@@ -293,6 +305,7 @@ public:
                     if (!advanceOneInputFrame(ringBuffer, channels)) {
                         m_underrunFrames += (outputFrames - outFrame);
                         m_totalOutputFramesGenerated += outFrame;
+                        unprime();
                         return outFrame;
                     }
                 }
@@ -314,6 +327,7 @@ public:
                     if (!advanceOneInputFrame(ringBuffer, channels)) {
                         m_underrunFrames += (outputFrames - outFrame);
                         m_totalOutputFramesGenerated += outFrame;
+                        unprime();
                         return outFrame;
                     }
                 }
@@ -622,7 +636,11 @@ public:
         if (framesProduced < outputFrames) [[unlikely]] {
+            m_resampler.unprime();
             m_fadeEngine.emergencyFadeOut(output, framesProduced, outputFrames, channels);
         } else {
+            if (m_fadeEngine.isMuted()) [[unlikely]] {
+                m_fadeEngine.startFadeIn();
+            }
             m_fadeEngine.process(output, outputFrames, channels);
         }
```

---

## 5. Dedicated Unit Test Implementation Template

```cpp
// ==============================================================================
// 11. Starvation Underrun & Recovery: Clean Un-Mute and Resampler Re-Priming
// ==============================================================================
TEST_CASE("drift_resampler_starvation_recovery_unmutes_and_resumes") {
    vban::ClockDriftConfig cfg;
    cfg.ringBufferCapacity = 1024;
    cfg.targetOccupancyPercent = 50.0;
    vban::ClockDriftCompensator comp(cfg);
    comp.reset();

    vban::SpscRingBuffer<float, 1024> ringBuffer;
    const size_t kChannels = 2;
    const size_t kBlockFrames = 128;

    // --- Phase 1: Healthy Streaming (Active Playback) ---
    // Feed 256 frames of constant 0.8f audio into the ring buffer.
    std::vector<float> healthyAudio(256 * kChannels, 0.8f);
    size_t written = ringBuffer.write(healthyAudio.data(), healthyAudio.size());
    ASSERT_EQ(written, healthyAudio.size());

    // Trigger initial fade-in and render first block
    comp.triggerFadeIn();
    std::vector<float> outBuf1(kBlockFrames * kChannels, 0.0f);
    size_t rendered1 = comp.process(ringBuffer, outBuf1.data(), kBlockFrames, kChannels);
    ASSERT_EQ(rendered1, kBlockFrames);
    ASSERT_TRUE(comp.getResampler().isPrimed());
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    // Fading in from 0.0 to 0.8: tail sample should be close to 0.8f
    ASSERT_GT(std::abs(outBuf1[(kBlockFrames - 1) * kChannels]), 0.5f);

    // Render second block: unity gain pass-through
    std::vector<float> outBuf2(kBlockFrames * kChannels, 0.0f);
    size_t rendered2 = comp.process(ringBuffer, outBuf2.data(), kBlockFrames, kChannels);
    ASSERT_EQ(rendered2, kBlockFrames);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_NEAR(outBuf2[i], 0.8f, 0.05f);
    }

    // --- Phase 2: Complete Starvation Dropout ---
    // Ring buffer is now completely empty (0 frames available).
    ASSERT_EQ(ringBuffer.availableRead(), 0U);

    std::vector<float> outBufStarve(kBlockFrames * kChannels, 1.0f);
    size_t renderedStarve = comp.process(ringBuffer, outBufStarve.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedStarve, 0U);

    // Emergency fade out MUST have muted the engine
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::Muted);

    // Resampler MUST be unprimed to prevent interpolating across dropout gap
    ASSERT_FALSE(comp.getResampler().isPrimed());

    // Buffer MUST be filled with exact silence (0.0f)
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_EQ(outBufStarve[i], 0.0f);
    }

    // Secondary starvation burst while still empty: remains muted & unprimed
    std::vector<float> outBufStarve2(kBlockFrames * kChannels, 1.0f);
    size_t renderedStarve2 = comp.process(ringBuffer, outBufStarve2.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedStarve2, 0U);
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    ASSERT_FALSE(comp.getResampler().isPrimed());
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_EQ(outBufStarve2[i], 0.0f);
    }

    // --- Phase 3: Buffer Refill & Recovery (The Critical Un-Mute Test) ---
    // Refill ring buffer with 512 frames of fresh audio (0.75f).
    std::vector<float> recoveredAudio(512 * kChannels, 0.75f);
    written = ringBuffer.write(recoveredAudio.data(), recoveredAudio.size());
    ASSERT_EQ(written, recoveredAudio.size());

    // Next audio callback: healthy frames resume.
    // ClockDriftCompensator MUST un-mute automatically and start fade-in!
    std::vector<float> outBufRecover1(kBlockFrames * kChannels, 0.0f);
    size_t renderedRecover1 = comp.process(ringBuffer, outBufRecover1.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedRecover1, kBlockFrames);

    // Resampler MUST have re-primed cleanly from fresh contiguous frames
    ASSERT_TRUE(comp.getResampler().isPrimed());

    // FadeEngine MUST have un-muted (not Muted!) and transitioned to Active across 128 samples
    ASSERT_FALSE(comp.getFadeEngine().isMuted());
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::Active);

    // Audio MUST start smoothly at 0.0f (cosine window start) and rise to 0.75f
    ASSERT_NEAR(outBufRecover1[0], 0.0f, 1e-4f);
    ASSERT_NEAR(outBufRecover1[1], 0.0f, 1e-4f);
    // Midpoint sample (index 64) should be ~ half amplitude (0.75 * 0.5 = 0.375)
    ASSERT_GT(outBufRecover1[64 * kChannels], 0.2f);
    // Final sample in window (index 127) should be near full amplitude
    ASSERT_NEAR(outBufRecover1[(kBlockFrames - 1) * kChannels], 0.75f, 0.05f);

    // --- Phase 4: Subsequent Blocks (Full Unity Gain Audio) ---
    std::vector<float> outBufRecover2(kBlockFrames * kChannels, 0.0f);
    size_t renderedRecover2 = comp.process(ringBuffer, outBufRecover2.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedRecover2, kBlockFrames);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_NEAR(outBufRecover2[i], 0.75f, 0.05f);
    }

    // --- Phase 5: Partial Block (Sub-128 Frame) Recovery Multi-Step Transition ---
    // Drain ring buffer to test multi-step transition with 64-frame callbacks
    while (ringBuffer.availableRead() > 0) {
        float dummy[64];
        ringBuffer.read(dummy, std::min(ringBuffer.availableRead(), size_t{64}));
    }
    ASSERT_EQ(ringBuffer.availableRead(), 0U);

    std::vector<float> outBufStarve3(64 * kChannels, 1.0f);
    size_t renderedStarve3 = comp.process(ringBuffer, outBufStarve3.data(), 64, kChannels);
    ASSERT_EQ(renderedStarve3, 0U);
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    ASSERT_FALSE(comp.getResampler().isPrimed());

    // Refill buffer with audio (0.6f)
    std::vector<float> subBlockAudio(512 * kChannels, 0.6f);
    ringBuffer.write(subBlockAudio.data(), subBlockAudio.size());

    // Callback 1 (64 frames): should transition to FadingIn and remain FadingIn (halfway)
    std::vector<float> outBuf64_1(64 * kChannels, 0.0f);
    size_t rendered64_1 = comp.process(ringBuffer, outBuf64_1.data(), 64, kChannels);
    ASSERT_EQ(rendered64_1, 64U);
    ASSERT_TRUE(comp.getResampler().isPrimed());
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::FadingIn);
    ASSERT_TRUE(comp.getFadeEngine().isTransitioning());
    ASSERT_NEAR(outBuf64_1[0], 0.0f, 1e-4f);

    // Callback 2 (64 frames): completes the 128-sample cosine window -> becomes Active
    std::vector<float> outBuf64_2(64 * kChannels, 0.0f);
    size_t rendered64_2 = comp.process(ringBuffer, outBuf64_2.data(), 64, kChannels);
    ASSERT_EQ(rendered64_2, 64U);
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::Active);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    // End of block 2 is at full amplitude
    ASSERT_NEAR(outBuf64_2[(64 - 1) * kChannels], 0.6f, 0.05f);

    // Callback 3 (64 frames): full amplitude unity gain
    std::vector<float> outBuf64_3(64 * kChannels, 0.0f);
    size_t rendered64_3 = comp.process(ringBuffer, outBuf64_3.data(), 64, kChannels);
    ASSERT_EQ(rendered64_3, 64U);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    for (size_t i = 0; i < 64 * kChannels; ++i) {
        ASSERT_NEAR(outBuf64_3[i], 0.6f, 0.05f);
    }
}
```

---

## 6. Verification and Validation Plan

When the worker/implementer applies these changes:
1. **Compile & Execute Standalone C++20 Test Harness**:
   ```powershell
   cmake --build native_tests/build --config Release
   .\native_tests\build\vban_native_tests.exe
   ```
   Expected result: **78/78 passed, 0 failed** (includes new test `drift_resampler_starvation_recovery_unmutes_and_resumes`).

2. **Execute Full End-to-End Test Suite**:
   ```powershell
   python e2e_tests/run_tests.py
   ```
   Expected result: **84/84 test cases passed**.

3. **Android Gradle Build**:
   ```powershell
   .\gradlew.bat assembleDebug
   ```
   Expected result: **BUILD SUCCESSFUL**.
