## 2026-10-18T14:52:45Z
You are the Implementation Worker for Milestone 2: Native VBAN Core (Protocol, Formats & SPSC Ring Buffer) for VBAN Receptor.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\worker_m2
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Scope Document: D:\Workspaces\vban-receptor\.agents\teamwork\sub_orch_m2\SCOPE.md
- Protocol Explorer Report & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m2_protocol\survey_report.md
- Dequantizer Explorer Report & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m2_converters\survey_report.md
- SPSC Buffer Explorer Report & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m2_spsc\survey_report.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

EXCLUSIVE WRITE OWNERSHIP (You own ONLY these files, do NOT touch other files):
- app/src/main/cpp/include/vban_protocol.hpp
- app/src/main/cpp/include/vban_parser.hpp
- app/src/main/cpp/src/vban_parser.cpp
- app/src/main/cpp/include/vban_converter.hpp
- app/src/main/cpp/src/vban_converter.cpp
- app/src/main/cpp/include/spsc_ring_buffer.hpp
- app/src/main/cpp/include/vban_engine.hpp
- app/src/main/cpp/src/vban_engine.cpp
- app/src/main/cpp/CMakeLists.txt
- native_tests/CMakeLists.txt
- native_tests/test_main.cpp
- native_tests/test_vban_protocol.cpp
- native_tests/test_vban_converter.cpp
- native_tests/test_spsc_buffer.cpp

OBJECTIVE & TASKS:
1. Implement the VBAN Protocol Core:
   - Create `app/src/main/cpp/include/vban_protocol.hpp` with packed 28-byte `VBanHeader`, FourCC magic `0x4E414256`, 21 official sample rates + 768 kHz extension (`VBAN_SR_TABLE`), format enums, and calculation helpers.
   - Create `app/src/main/cpp/include/vban_parser.hpp` and `app/src/main/cpp/src/vban_parser.cpp` with the strict 10-step packet validation pipeline, case-insensitive stream matching, 32-bit wrap-safe sequence tracking, and RFC 3550 EWMA jitter estimation.
2. Implement Audio Dequantizers:
   - Create `app/src/main/cpp/include/vban_converter.hpp` and `app/src/main/cpp/src/vban_converter.cpp` for PCM 16-bit, PCM 24-bit packed (with sign extension), PCM 32-bit, and Float32 sanitization (NaN/Inf to 0.0f, clamping to `[-1.0f, +1.0f]`), plus channel interleaving/deinterleaving.
3. Implement the Lock-Free SPSC Circular Ring Buffer:
   - Create `app/src/main/cpp/include/spsc_ring_buffer.hpp` implementing `SpscRingBuffer<T, Capacity>` with 64-byte alignment (`alignas(64)`), monotonic atomic indices, local index caching, zero runtime allocations, and contiguous vectorized block transfers.
4. Update Native Engine & CMake:
   - Update `app/src/main/cpp/CMakeLists.txt` to add `src/vban_parser.cpp` and `src/vban_converter.cpp` to the `vban_engine` target.
   - Update `app/src/main/cpp/include/vban_engine.hpp` and `app/src/main/cpp/src/vban_engine.cpp` to integrate parser, converters, and SPSC buffer.
5. Implement Modular Native Unit Tests:
   - Create `native_tests/test_vban_protocol.cpp`, `native_tests/test_vban_converter.cpp`, and `native_tests/test_spsc_buffer.cpp`.
   - Update `native_tests/CMakeLists.txt` and `native_tests/test_main.cpp` to run the modular test suites.
6. Run Builds & Tests:
   - Rebuild and run standalone native tests:
     `cmake -S native_tests -B native_tests/build -G Ninja -DCMAKE_MAKE_PROGRAM="D:/Android/sdk/cmake/3.22.1/bin/ninja.exe"`
     `cmake --build native_tests/build`
     `.\native_tests\build\vban_native_tests.exe`
     Confirm all tests pass (0 failures).
   - Rebuild Android APK:
     `.\gradlew.bat assembleDebug`
     Confirm successful APK packaging with native libraries.
7. Documentation & Handoff:
   - Write standard 5-component handoff report to `D:\Workspaces\vban-receptor\.agents\teamwork\worker_m2\handoff.md`.
   - Update `progress.md` in your directory.
   - Send completion message to parent orchestrator (Recipient: 9bb0c473-fb0a-499a-bbf2-a430786d0058).
