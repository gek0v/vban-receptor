# DISPATCH — Milestone 4 Remediation Explorer

## Objective
Analyze Reviewer 1's findings from Milestone 4 Iteration 1 and design production-ready C++20 remediation diffs and tests:
1. Flaky native test `jni_telemetry_concurrent_snapshot_integrity` in `native_tests/test_vban_receiver.cpp`: Replace unthrottled loop racing with a bounded reader synchronization model (e.g. `while (readSnapshots.load() < 200)` or thread yields) and test genuine `VbanReceiver`/`VbanParser` snapshots.
2. Data race on `VbanParser` fields: Update `app/src/main/cpp/include/vban_parser.hpp` and `src/vban_parser.cpp` to make `totalPackets_`, `validPackets_`, `lostPackets_`, `outOfOrderPackets_`, `lastNuFrame_`, and `jitterSeconds_` thread-safe (atomics or atomic snapshot struct).
3. SPSC partial-sample write on buffer overflow swapping channels: In `VbanReceiver::processDatagram`, ensure whole-frame alignment before writing to `m_ringBuffer` so that partial frames are NEVER written, preventing Left/Right channel swap under buffer saturation.
4. Data race on `m_socketFd` during shutdown: Make `m_socketFd` an `std::atomic<intptr_t>`.
5. Unsynchronized `currentConfig_` access in `VbanEngine`: Make `currentConfig_` fields atomic or protect with atomic snapshot.

## Mandatory Inputs
- Reviewer 1 Handoff: `D:\Workspaces\vban-receptor\.agents\teamwork\reviewer_m4_1\handoff.md`
- Target Code Files:
  - `app/src/main/cpp/include/vban_parser.hpp`
  - `app/src/main/cpp/src/vban_parser.cpp`
  - `app/src/main/cpp/include/vban_receiver.hpp`
  - `app/src/main/cpp/src/vban_receiver.cpp`
  - `app/src/main/cpp/include/vban_engine.hpp`
  - `app/src/main/cpp/src/vban_engine.cpp`
  - `native_tests/test_vban_receiver.cpp`

## 2026-10-18T16:57:55Z
You are the Remediation Explorer for Milestone 4 Iteration 2: Network Receiver & JNI Telemetry Bridge.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_fix
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Reviewer 1 Handoff Report with Defect Details: D:\Workspaces\vban-receptor\.agents\teamwork\reviewer_m4_1\handoff.md
- Worker M4 Handoff Report: D:\Workspaces\vban-receptor\.agents\teamwork\worker_m4\handoff.md
- Target Source Files:
  - app/src/main/cpp/include/vban_parser.hpp
  - app/src/main/cpp/src/vban_parser.cpp
  - app/src/main/cpp/include/vban_receiver.hpp
  - app/src/main/cpp/src/vban_receiver.cpp
  - app/src/main/cpp/include/vban_engine.hpp
  - app/src/main/cpp/src/vban_engine.cpp
  - native_tests/test_vban_receiver.cpp

DEFECT DETAILS FROM REVIEWER 1:
1. Critical Finding: Flaky Native Test `jni_telemetry_concurrent_snapshot_integrity`
   Location: `native_tests/test_vban_receiver.cpp:382-435`
   Issue: `ASSERT_GT(readSnapshots.load(), 100U)` fails intermittently (6% failure rate over 50 runs) because writer thread completes 20,000 unthrottled memory stores in microseconds before the reader thread finishes 100 snapshots.
   Remediation: Make writer loop coordinate with reader (e.g. `while (readSnapshots.load() < 200 && ...)` or yield/micro-sleep per iteration) and test genuine `VbanReceiver`/`VbanParser` snapshots.
2. Major Finding: Data Race on `VbanParser` Telemetry Fields
   Location: `app/src/main/cpp/include/vban_parser.hpp:135-144` and `src/vban_parser.cpp:29-38`
   Issue: Non-atomic members (`totalPackets_`, `validPackets_`, `lostPackets_`, `outOfOrderPackets_`, `lastNuFrame_`, `jitterSeconds_`) are updated continuously by the network thread and concurrently read by JNI `getTelemetry()`.
   Remediation: Convert `totalPackets_`, `validPackets_`, `lostPackets_`, `outOfOrderPackets_`, `lastNuFrame_` to `std::atomic`, and protect/atomize `jitterSeconds_` (or store atomic telemetry snapshot).
3. Major Finding: SPSC Partial-Sample Write on Overrun Causes Permanent Channel Phase Swap
   Location: `app/src/main/cpp/src/vban_receiver.cpp:386-393`
   Issue: When `m_ringBuffer` does not have space for all `totalSamples`, `write()` writes partial frames if available space is not a multiple of channels. This permanently shifts left/right channel alignment in the circular buffer.
   Remediation: Verify available space in `m_ringBuffer` accommodates whole frames before writing, or truncate `samplesToWrite = available - (available % packet.channels)`. If `samplesToWrite < totalSamples`, account for dropped frames and ensure only complete frames are written.
4. Minor Finding: Data Race on `m_socketFd` during Receiver Teardown
   Location: `app/src/main/cpp/include/vban_receiver.hpp:190` and `src/vban_receiver.cpp:272, 422`
   Issue: Non-atomic `intptr_t m_socketFd` accessed concurrently during `stop()`.
   Remediation: Change to `std::atomic<intptr_t> m_socketFd{-1};`.
5. Minor Finding: Unsynchronized `currentConfig_` in `VbanEngine`
   Location: `app/src/main/cpp/include/vban_engine.hpp:176` and `src/vban_engine.cpp:238, 261`
   Issue: `currentConfig_` struct is written by `reconfigWorkerLoop` and read by `getTelemetry()` across threads without synchronization.
   Remediation: Store `m_currentSampleRate` and `m_currentChannels` as atomic integers, or protect configuration queries with an atomic snapshot.
6. Design a new unit test in `native_tests/test_vban_receiver.cpp` verifying that under buffer saturation, channel alignment is strictly preserved and Left/Right channels never swap.

OBJECTIVE:
Analyze these findings, formulate exact diffs/replacements, and provide complete C++20 code templates for:
- `vban_parser.hpp` / `vban_parser.cpp` update
- `vban_receiver.hpp` / `vban_receiver.cpp` update
- `vban_engine.hpp` / `vban_engine.cpp` update
- `native_tests/test_vban_receiver.cpp` updates (flaky test fix + buffer saturation channel alignment test)

Write your survey report to: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_fix\survey_report.md`
Write handoff report to: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_fix\handoff.md`
Update progress.md and notify parent orchestrator.
Do NOT modify any source code files directly outside your directory.
