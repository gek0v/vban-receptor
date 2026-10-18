## 2026-10-18T16:24:03Z

You are an Explorer agent for Milestone 4: JNI Bridge & Kotlin Wrapper Architecture.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_jni
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Scope Document: D:\Workspaces\vban-receptor\PROJECT.md § Interface Contracts & Milestones (M4)

OBJECTIVE:
Investigate and design production-ready code templates for the JNI bridge, Kotlin native engine wrapper, and native unit tests:
1. JNI Bridge Implementation (`app/src/main/cpp/src/jni_bridge.cpp`):
   - JNI function exports:
     - `Java_com_vban_receptor_native_NativeVbanEngine_initEngine(JNIEnv*, jobject)`
     - `Java_com_vban_receptor_native_NativeVbanEngine_startReceiver(JNIEnv*, jobject, jstring listenIp, jint port, jstring streamName, jint deviceId)`
     - `Java_com_vban_receptor_native_NativeVbanEngine_stopReceiver(JNIEnv*, jobject)`
     - `Java_com_vban_receptor_native_NativeVbanEngine_setAudioDevice(JNIEnv*, jobject, jint deviceId)`
     - `Java_com_vban_receptor_native_NativeVbanEngine_getTelemetry(JNIEnv*, jobject) -> jobject`
   - Non-blocking snapshot retrieval of `NativeTelemetry` fields:
     - `sampleRate`, `channelCount`, `bitDepth`, `totalPacketsReceived`, `lostPackets`, `jitterMs`, `bufferOccupancyPercent`, `peakLevelLeft`, `peakLevelRight`.
2. Kotlin Wrapper (`app/src/main/java/com/vban/receptor/native/NativeVbanEngine.kt`):
   - Data class `NativeTelemetry` matching the JNI constructor.
   - Thread-safe singleton or lifecycle-aware wrapper managing `System.loadLibrary("vban_engine")`.
3. Standalone Host Test Suite (`native_tests/test_vban_receiver.cpp`):
   - Design host-runnable C++ unit tests verifying:
     - UDP socket bind, 4MB buffer configuration, and teardown.
     - Loopback packet sending and receiving through `VbanReceiver`.
     - Multicast address validation.
     - JNI telemetry struct field packing and non-blocking snapshot integrity.
4. Output:
   - Write comprehensive survey findings and complete code templates to D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_jni\survey_report.md.
   - Write handoff report to D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_jni\handoff.md.
   - Update progress.md and notify parent orchestrator.
Do NOT modify any source code files directly outside your directory.
