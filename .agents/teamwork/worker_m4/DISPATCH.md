## 2026-10-18T16:30:48Z

You are the Implementation Worker for Milestone 4: Network Receiver & JNI Telemetry Bridge for VBAN Receptor.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\worker_m4
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Scope Document: D:\Workspaces\vban-receptor\PROJECT.md § Interface Contracts & Milestones (M4)
- Network Explorer Survey & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_network\survey_report.md
- Pipeline Explorer Survey & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_pipeline\survey_report.md
- JNI Explorer Survey & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_jni\survey_report.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

EXCLUSIVE WRITE OWNERSHIP (You own ONLY these files, do NOT touch other files):
- app/src/main/cpp/include/vban_receiver.hpp
- app/src/main/cpp/src/vban_receiver.cpp
- app/src/main/cpp/include/vban_engine.hpp
- app/src/main/cpp/src/vban_engine.cpp
- app/src/main/cpp/src/jni_bridge.cpp
- app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt
- app/src/main/cpp/CMakeLists.txt
- native_tests/test_vban_receiver.cpp
- native_tests/CMakeLists.txt
- native_tests/test_main.cpp

OBJECTIVE & TASKS:
1. Implement UDP Socket & Receiver Engine (`vban_receiver.hpp`, `vban_receiver.cpp`):
   - Cross-platform socket abstraction (POSIX BSD sockets on Android NDK, Winsock guard on Windows).
   - UDP socket creation, `SO_REUSEADDR`, `SO_REUSEPORT` (where available), 4 MB `SO_RCVBUF` target with descending ladder {4MB, 2MB, 1MB, 512KB, 256KB}.
   - Class D IPv4 multicast address detection (224.0.0.0 to 239.255.255.255), `ip_mreq` setup, `IP_ADD_MEMBERSHIP` and clean `IP_DROP_MEMBERSHIP`.
   - `SO_RCVTIMEO` (100 ms) and `shutdown(SHUT_RDWR)` for instant clean thread termination without deadlock.
   - High-priority network worker thread receive loop: 16 KB stack receive buffer, zero heap allocations, packet parsing via `VbanParser`, sequence wrap tracking, RFC 3550 EWMA jitter estimation, Float32 dequantization via `VbanConverter`, and atomic push into `SpscRingBuffer<float, 262144>`.
   - Decoupled dynamic stream reconfiguration: triggers 128-sample cosine fade-out, `OboePlayer` reconfiguration, and cosine fade-in when incoming sample rate or channel count changes.
2. Implement Modular JNI Bridge (`app/src/main/cpp/src/jni_bridge.cpp`):
   - JNI function exports: `initEngine`, `startReceiver(listenIp, port, streamName, deviceId)`, `stopReceiver`, `setAudioDevice(deviceId)`, and non-blocking `getTelemetry`.
   - `JNI_OnLoad` caching `g_telemetryClass` and `g_telemetryCtor` descriptor `(IIIJJFFFF)V` via `env->NewGlobalRef` to eliminate per-poll reflection overhead.
   - Clean up duplicate preliminary JNI stubs from `vban_engine.cpp` (lines 260-330).
3. Create Kotlin Wrapper (`app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt`):
   - Data class `NativeTelemetry` matching the 9 fields and JNI constructor.
   - Thread-safe singleton wrapper loading `vban_engine` and exposing reactive `StateFlow` streams.
4. Implement Native Unit Tests (`native_tests/test_vban_receiver.cpp`):
   - Standalone host unit tests covering socket bind, 4MB buffer configuration, loopback UDP roundtrip through receiver, multicast address detection, and 40-byte `NativeTelemetry` struct packing and concurrent snapshot integrity.
   - Update `native_tests/CMakeLists.txt` (including `ws2_32` link on Windows) and `native_tests/test_main.cpp`.
   - Update `app/src/main/cpp/CMakeLists.txt` to include `src/vban_receiver.cpp` and `src/jni_bridge.cpp`.
5. Run Builds and Tests:
   - Standalone native test runner:
     `cmake -S native_tests -B native_tests/build -G Ninja -DCMAKE_MAKE_PROGRAM="D:/Android/sdk/cmake/3.22.1/bin/ninja.exe"`
     `cmake --build native_tests/build`
     `.\native_tests\build\vban_native_tests.exe`
     Confirm all tests pass (0 failures).
   - Android debug APK build:
     `.\gradlew.bat assembleDebug`
     Confirm clean APK build and packaging.
   - Run E2E test suite:
     `python e2e_tests/run_tests.py`
     Confirm 84/84 tests pass.
6. Documentation & Handoff:
   - Write standard 5-component handoff report to `D:\Workspaces\vban-receptor\.agents\teamwork\worker_m4\handoff.md`.
   - Update `progress.md`.
   - Send completion message to parent orchestrator (Recipient: 9bb0c473-fb0a-499a-bbf2-a430786d0058).
