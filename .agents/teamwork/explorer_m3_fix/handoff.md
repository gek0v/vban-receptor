# Handoff Report: Starvation Recovery Mute Latch & Resampler Re-Priming Remediation

**Agent**: Explorer M3 Fix (`explorer_m3_fix`)  
**Role**: Explorer / Problem Analysis & Specification  
**Parent Orchestrator ID**: `9bb0c473-fb0a-499a-bbf2-a430786d0058`  
**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m3_fix`  
**Date**: 2026-10-18T15:53:00Z  
**Type**: Hard (Task Complete)  

---

## 1. Observation

1. **Reviewer 1 Defect Report**:
   - `D:\Workspaces\vban-receptor\.agents\teamwork\reviewer_m3_1\handoff.md` (lines 43–86, Finding 1):
     ```
     When a momentary buffer underrun occurs (framesProduced < outputFrames), emergencyFadeOut sets m_state = FadeState::Muted.
     When healthy packets resume and the ring buffer is replenished (framesProduced == outputFrames),
     ClockDriftCompensator::process invokes m_fadeEngine.process(output, outputFrames, channels).
     Because m_state is FadeState::Muted, FadeEngine::process zeroes out the buffer with std::memset
     and remains in FadeState::Muted indefinitely. No subsequent audio is ever heard.
     ```
   - Lines 89–96 (Finding 2):
     ```
     When the ring buffer starves completely, m_isPrimed remains true in CatmullRomResampler.
     If thousands of frames elapse before packets resume, the first output sample interpolates
     between the stale frame from before the dropout and the new frame.
     ```

2. **Existing Implementation Code**:
   - In `app/src/main/cpp/include/drift_compensator.hpp`:
     - Lines 214–215:
       ```cpp
       m_phase = 0.0;
       m_isPrimed = false;
       ```
     - Lines 293–298:
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
     - Lines 622–627:
       ```cpp
       if (framesProduced < outputFrames) [[unlikely]] {
           m_fadeEngine.emergencyFadeOut(output, framesProduced, outputFrames, channels);
       } else {
           m_fadeEngine.process(output, outputFrames, channels);
       }
       ```
   - In `app/src/main/cpp/src/drift_compensator.cpp`:
     - Lines 34–37:
       ```cpp
       if (m_state == FadeState::Muted) {
           std::memset(buffer, 0, frames * channels * sizeof(float));
           return;
       }
       ```
     - Lines 91–92:
       ```cpp
       m_state = FadeState::Muted;
       m_sampleIndex = FADE_WINDOW_SIZE;
       ```

3. **Baseline Verification Commands**:
   - Executed native tests runner:
     `.\native_tests\build\vban_native_tests.exe`
     Output: `Summary: 77/77 passed, 0 failed (66.000 ms total)`.

---

## 2. Logic Chain

1. In `FadeEngine::emergencyFadeOut`, when an underrun occurs (`framesProduced < outputFrames`), `m_state` is unconditionally transitioned to `FadeState::Muted` and `m_sampleIndex` is set to `FADE_WINDOW_SIZE` (128).
2. When healthy frames resume, `m_resampler.process(...)` produces `outputFrames` frames (`framesProduced == outputFrames`).
3. `ClockDriftCompensator::process` invokes `m_fadeEngine.process(output, outputFrames, channels)`.
4. In `FadeEngine::process`, lines 34–37 state:
   ```cpp
   if (m_state == FadeState::Muted) {
       std::memset(buffer, 0, frames * channels * sizeof(float));
       return;
   }
   ```
   Because `m_state` is `FadeState::Muted`, `FadeEngine::process` zeroes the newly rendered audio buffer with `std::memset` and returns without changing `m_state`.
5. Since neither `ClockDriftCompensator::process` nor `FadeEngine::process` ever invokes `startFadeIn()` when healthy frames arrive, `m_state` remains `FadeState::Muted` indefinitely. This creates a permanent mute latch.
6. Adding `if (m_fadeEngine.isMuted()) [[unlikely]] { m_fadeEngine.startFadeIn(); }` in `ClockDriftCompensator::process` before `m_fadeEngine.process(...)` breaks the latch:
   - If muted, `startFadeIn()` sets `m_sampleIndex = 0` and `m_state = FadeState::FadingIn`.
   - `m_fadeEngine.process` multiplies the audio by the 128-sample raised cosine window, transitioning to `FadeState::Active` once 128 samples are processed.
   - For sub-128 buffers (e.g. 64 frames), `isMuted()` is false during block 2, allowing smooth multi-callback completion of the fade window.
7. For Finding 2, when `advanceOneInputFrame` fails during ring buffer exhaustion, `m_isPrimed` remains `true` in `CatmullRomResampler`. Stale history samples $p_0, p_1, p_2, p_3$ persist across dropouts.
8. Adding `unprime()` to `CatmullRomResampler` (clearing `m_history`, `m_phase`, `m_stagingAvailable`, `m_stagingReadPos`, and setting `m_isPrimed = false`) and invoking it on underrun ensures that upon recovery, `primeFromRingBuffer` cleanly seeds all 4 history slots with contiguous incoming frames from the refilled stream.

---

## 3. Caveats

- `ClockDriftCompensator::process` is a C++ template method located in the header file `drift_compensator.hpp`. No changes are required in `drift_compensator.cpp`.
- The audio callback runs in AAudio high-priority thread context; `startFadeIn()` and `unprime()` execute in $O(1)$ time with zero dynamic allocations and zero locks, strictly maintaining real-time safety.
- The unit test requires the test runner executable to be recompiled after source changes.

---

## 4. Conclusion

The permanent mute latch and stale resampler history defects are fully diagnosed and remediated:
1. **Source Modifications**:
   - `app/src/main/cpp/include/drift_compensator.hpp`:
     - Add `unprime()` method to `CatmullRomResampler` and refactor `reset()`.
     - Call `unprime()` in `CatmullRomResampler::process` when `advanceOneInputFrame` fails.
     - Call `m_resampler.unprime()` and `if (m_fadeEngine.isMuted()) m_fadeEngine.startFadeIn();` in `ClockDriftCompensator::process`.
2. **Unit Test Implementation**:
   - `native_tests/test_drift_resampler.cpp`:
     - Implement Test 11 `drift_resampler_starvation_recovery_unmutes_and_resumes` covering active streaming, complete underrun starvation, silence enforcement, automatic un-mute recovery, cosine fade-in verification, and multi-callback partial block progression.

All exact diffs and complete code templates are documented in `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m3_fix\survey_report.md`.

---

## 5. Verification Method

To verify the remediation once applied by the worker:
1. Build and run native test runner:
   ```powershell
   cmake --build native_tests/build --config Release
   .\native_tests\build\vban_native_tests.exe
   ```
   **Pass Condition**: 78/78 tests pass (including new `drift_resampler_starvation_recovery_unmutes_and_resumes`).

2. Run full End-to-End test suite:
   ```powershell
   python e2e_tests/run_tests.py
   ```
   **Pass Condition**: 84/84 tests pass.

3. Build Android debug APK:
   ```powershell
   .\gradlew.bat assembleDebug
   ```
   **Pass Condition**: `BUILD SUCCESSFUL`.
