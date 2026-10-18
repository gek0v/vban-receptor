## 2026-10-18T17:05:36Z
You are the Implementation Worker for Milestone 4 Iteration 2: Network Receiver & JNI Telemetry Bridge Remediation.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\worker_m4_fix
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Reviewer 1 Defect Report: D:\Workspaces\vban-receptor\.agents\teamwork\reviewer_m4_1\handoff.md
- Remediation Explorer Survey Report & Diffs: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_fix\survey_report.md
- Remediation Explorer Handoff: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_fix\handoff.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

EXCLUSIVE WRITE OWNERSHIP (You own ONLY these files, do NOT touch other files):
- app/src/main/cpp/include/vban_parser.hpp
- app/src/main/cpp/src/vban_parser.cpp
- app/src/main/cpp/include/vban_receiver.hpp
- app/src/main/cpp/src/vban_receiver.cpp
- app/src/main/cpp/include/vban_engine.hpp
- app/src/main/cpp/src/vban_engine.cpp
- native_tests/test_vban_receiver.cpp

OBJECTIVES & TASKS:
1. Apply atomic state tracking to `app/src/main/cpp/include/vban_parser.hpp` and `src/vban_parser.cpp` per `explorer_m4_fix/survey_report.md § 4.1 & 4.2`:
   - `std::atomic<uint64_t> totalPackets_`, `validPackets_`, `lostPackets_`, `outOfOrderPackets_`
   - `std::atomic<uint32_t> lastNuFrame_`
   - `std::atomic<double> jitterSeconds_`
   - Custom move constructor and move assignment operator for `VbanParser`
2. Apply whole-frame truncation and atomic socket management to `app/src/main/cpp/include/vban_receiver.hpp` and `src/vban_receiver.cpp` per `explorer_m4_fix/survey_report.md § 4.3 & 4.4`:
   - `std::atomic<intptr_t> m_socketFd{-1};` with atomic exchange in `closeSocket()`
   - In `processDatagram()`:
     ```cpp
     const size_t available = m_ringBuffer.availableWrite();
     const size_t wholeFrameAvailable = (available / packet.channels) * packet.channels;
     const size_t samplesToWrite = std::min(totalSamples, wholeFrameAvailable);
     if (samplesToWrite > 0) {
         m_ringBuffer.write(m_stagingBuffer.data(), samplesToWrite);
     }
     if (samplesToWrite < totalSamples) [[unlikely]] {
         const size_t droppedSamples = totalSamples - samplesToWrite;
         const size_t droppedFrames = droppedSamples / packet.channels;
         m_overflowFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
     }
     ```
3. Apply atomic stream configuration to `app/src/main/cpp/include/vban_engine.hpp` and `src/vban_engine.cpp` per `explorer_m4_fix/survey_report.md § 4.5 & 4.6`:
   - `std::atomic<uint32_t> m_currentSampleRate` and `std::atomic<uint16_t> m_currentChannels`
4. Update `native_tests/test_vban_receiver.cpp` per `explorer_m4_fix/survey_report.md § 4.7`:
   - Fix flaky `jni_telemetry_concurrent_snapshot_integrity` test with synchronized reader/writer loop using real `VbanReceiver`.
   - Implement new test `receiver_buffer_saturation_channel_alignment_preservation` verifying channel phase and alignment under buffer saturation.
5. Rebuild and Run Verification:
   - Build native tests: `cmake --build native_tests/build`
   - Run native test runner: `.\native_tests\build\vban_native_tests.exe` (all 89 tests must pass).
   - Run 50-iteration stability loop:
     `$fail = 0; for ($i=1; $i -le 50; $i++) { .\native_tests\build\vban_native_tests.exe | Out-Null; if ($LASTEXITCODE -ne 0) { $fail++ } }; Write-Output "Failures: $fail / 50"`
     Verify `Failures: 0 / 50`.
   - Rebuild Android APK: `.\gradlew.bat assembleDebug` (must be BUILD SUCCESSFUL).
   - Run E2E test suite: `python e2e_tests/run_tests.py` (must be 84/84 PASS).
6. Deliver standard 5-component handoff report to `D:\Workspaces\vban-receptor\.agents\teamwork\worker_m4_fix\handoff.md` and notify parent orchestrator.
