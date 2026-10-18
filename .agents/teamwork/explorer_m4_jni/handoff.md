# Handoff Report: Milestone 4 - JNI Bridge & Kotlin Wrapper Architecture

**Agent**: explorer_m4_jni  
**Type**: Hard Handoff (Investigation & Architecture Design Complete)  
**Parent Agent**: 9bb0c473-fb0a-499a-bbf2-a430786d0058  
**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_jni`  

---

## 1. Observation

Direct code and environment observations:
1. **Existing JNI Implementation**: In `app/src/main/cpp/src/vban_engine.cpp` (lines 260-330), preliminary JNI functions were placed at the end of the file. However, `PROJECT.md` (lines 205-212) defines a dedicated modular structure with `app/src/main/cpp/src/jni_bridge.cpp`. In `app/src/main/cpp/CMakeLists.txt`, `jni_bridge.cpp` and `vban_receiver.cpp` are not yet included in the `vban_engine` target sources.
2. **NativeTelemetry Contract**:
   - In `app/src/main/cpp/include/vban_engine.hpp` (lines 43-53), `NativeTelemetry` is defined with 9 fields: `sampleRate` (uint32_t), `channelCount` (uint16_t), `bitDepth` (uint16_t), `totalPacketsReceived` (uint64_t), `lostPackets` (uint64_t), `jitterMs` (float), `bufferOccupancyPercent` (float), `peakLevelLeft` (float), `peakLevelRight` (float). Total struct size is 40 bytes with natural alignment.
   - The corresponding JNI constructor descriptor is `"(IIIJJFFFF)V"`.
3. **JNI Reflection Overhead & Concurrency**:
   - In `vban_engine.cpp` lines 304-314, `env->FindClass` and `env->GetMethodID` were called dynamically inside `getTelemetry()` on every invocation without caching. When polled at 30-60 Hz from UI/service threads, this incurs unnecessary reflection overhead.
   - `telemetryCache_` in `vban_engine.hpp` was declared as a mutable member struct, which introduces data race risks when written from the packet processing thread while read from JNI.
4. **Kotlin Wrapper Absence**:
   - Directory `app/src/main/java` does not exist yet. `NativeVbanEngine.kt` and `NativeTelemetry` data class must be established under `app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt`.
5. **Host Test Suite**:
   - `native_tests/` currently executes 81 passing unit tests (all passing in ~65 ms).
   - `test_vban_receiver.cpp` does not yet exist. `native_tests/CMakeLists.txt` needs conditional inclusion of `test_vban_receiver.cpp` and linkage of `ws2_32` on Windows hosts.

---

## 2. Logic Chain

1. **Separation of Concerns**: Moving JNI exports into `src/jni_bridge.cpp` aligns with `PROJECT.md` § Code Layout and decouples Android NDK JNI machinery from the core audio coordinator (`vban_engine.cpp`).
2. **JNI Reflection Optimization via Global References**:
   - By implementing `JNI_OnLoad` and caching `g_telemetryClass` as an `env->NewGlobalRef` alongside `g_telemetryCtor`, class lookup is performed once at library load time.
   - Calling `env->NewObject` using cached identifiers reduces per-frame telemetry overhead from several microseconds to nanoseconds, ensuring zero GC pressure and immunity from background thread ClassLoader lookup failures.
3. **Thread Safety & Zero Data Race**:
   - `NativeTelemetry` is polled by Kotlin coroutines concurrently while the network thread receives packets and the Oboe callback renders audio.
   - Generating `NativeTelemetry` snapshots by reading atomics (`parser_.getTelemetry()`, `ringBuffer_.occupancyPercent()`, and `player_->getPeakLevelLeft()`) completely eliminates data races without acquiring any mutex on the audio path.
4. **Kotlin Wrapper Architecture**:
   - An `object NativeVbanEngine` singleton provides thread-safe lifecycle control, wraps `System.loadLibrary("vban_engine")`, and exposes reactive `StateFlow<Boolean>` (`isRunning`) and `StateFlow<NativeTelemetry>` (`telemetry`) for Jetpack Compose and Foreground Service consumers (M5).
5. **Host Unit Test Conformance**:
   - By creating `native_tests/test_vban_receiver.cpp` with scoped `WsaGuard` (Winsock RAII on Windows, POSIX on Linux/Android), the test harness can exercise UDP socket creation, 4MB `SO_RCVBUF` buffer configuration, loopback datagram roundtrip, multicast address range checking, and 40-byte struct layout integrity natively in C++20.

---

## 3. Caveats

1. **Linux Kernel `SO_RCVBUF` Behavior**: In Linux/Android kernels, `getsockopt(..., SO_RCVBUF, ...)` returns `2 * requested_size` due to internal `sk_buff` bookkeeping overhead. Tests should verify `actual >= requested` or `actual > 64KB` rather than strict equality.
2. **Host Port Collisions**: When binding test sockets in `test_vban_receiver.cpp`, binding to port `0` (ephemeral port assigned by OS) guarantees zero test failures due to port collision with running services on the host machine.
3. **CMake Target Update Requirement**: The implementation agent must update both `app/src/main/cpp/CMakeLists.txt` and `native_tests/CMakeLists.txt` to include the new source and test files.

---

## 4. Conclusion

The architectural design and production-ready code templates for Milestone 4 (JNI Bridge, Kotlin Native Engine Wrapper, and Native Host Tests) are complete and documented in detail in `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_jni\survey_report.md`.
The implementation steps are scoped and ready for the implementer agent:
1. Create `app/src/main/cpp/src/jni_bridge.cpp` using the template in §3 of `survey_report.md`.
2. Remove duplicate JNI export functions from `app/src/main/cpp/src/vban_engine.cpp` (lines 260-330).
3. Create `app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt` using the template in §4 of `survey_report.md`.
4. Create `native_tests/test_vban_receiver.cpp` using the template in §5 of `survey_report.md`.
5. Update `app/src/main/cpp/CMakeLists.txt` and `native_tests/CMakeLists.txt` as detailed in §6 of `survey_report.md`.

---

## 5. Verification Method

To independently verify the implementation:
1. **Host Native Test Suite**:
   ```powershell
   cmake -B native_tests/build -S native_tests
   cmake --build native_tests/build --config Release
   .\native_tests\build\vban_native_tests.exe
   ```
   *Expected Result*: All tests pass (including `receiver_socket_create_bind_and_buffer_config`, `receiver_loopback_udp_transmission_and_reception`, `receiver_multicast_address_range_validation`, `jni_telemetry_struct_packing_and_alignment`, `jni_telemetry_concurrent_snapshot_integrity`).
2. **Android Gradle Native Build**:
   ```powershell
   .\gradlew.bat compileDebugSources
   ```
   *Expected Result*: Compiles `jni_bridge.cpp` into `libvban_engine.so` and compiles `NativeVbanEngine.kt` without errors.
3. **JNI Symbol Visibility Check**:
   On the compiled shared object:
   Verify `Java_com_vban_receptor_native_NativeVbanEngine_*` symbols are exported with `GLOBAL` / `DEFAULT` visibility.
