# Handoff Report: NDK & CMake C++20 Build Configuration (Milestone 1)

**From**: Explorer Agent (`explorer_m1_cmake`)  
**To**: Parent Orchestrator (`9bb0c473-fb0a-499a-bbf2-a430786d0058`) / Milestone 1 Sub-Orchestrator  
**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m1_cmake`  
**Date**: 2026-10-18  
**Milestone**: M1 - Build Scaffolding & Native Test Harness  

---

## 1. Observation

1. **Host NDK Toolchain**:
   - Path `D:\Android\sdk\ndk\26.1.10909125` contains Clang 17.0.2:
     ```
     Android (10552028, based on r487747d) clang version 17.0.2
     Target: x86_64-w64-windows-gnu
     ```
   - Target compiler wrapper `D:\Android\sdk\ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android26-clang++.cmd` is installed and functional.
2. **Host CMake Version**:
   - `D:\Android\sdk\cmake\3.22.1` exists in the Android SDK directory.
   - System PATH CMake version is `4.4.4`.
3. **C++20 Compilation Experiment**:
   - Compiled and ran C++20 host test program with concepts (`template <typename T> concept AudioSampleType`), `std::endian::native`, and `std::span`. Command:
     `& "D:\Android\sdk\ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" -std=c++20 test_cxx20.cpp -o test_cxx20.exe`
     Result: Exited with code `0`.
4. **Target Shared Library Compilation Experiment**:
   - Compiled an ARM64-v8a shared library `libtest_jni.so` using `aarch64-linux-android26-clang++.cmd -std=c++20 -shared -fPIC ... -llog`.
     Result: Exited with code `0`, generated 5,664-byte `.so` verifying `<jni.h>`, `<android/log.h>`, and C++20 standard library headers.
5. **Specification Constraints**:
   - `PROJECT.md` lines 123-127 mandate C++20 (`-std=c++20`), `arm64-v8a` and `x86_64`, `c++_shared`, and `oboe::oboe` target via `find_package(oboe REQUIRED CONFIG)`.
   - `PROJECT.md` lines 148-174 mandate JNI signatures for `startReceiver`, `stopReceiver`, `setAudioDevice`, and `getTelemetry`.
   - `SCOPE.md` lines 14-17 mandate `cmake_minimum_required(VERSION 3.22.1)`, `project("vban_engine" CXX)`, `set(CMAKE_CXX_STANDARD 20)`, `set(CMAKE_CXX_STANDARD_REQUIRED ON)`, linking `oboe::oboe`, `android`, `log`.

---

## 2. Logic Chain

1. **Toolchain Capability**:
   - Observation 1 and 3 prove that NDK 26.1 (Clang 17.0.2) fully supports ISO C++20 standard libraries and language features without requiring third-party backports.
2. **Android Target Compatibility**:
   - Observation 4 proves that the NDK toolchain cleanly compiles and links Android shared libraries against API level 26 (Android 8.0+) with JNI headers and the Android logging library (`liblog.so`).
3. **Prefab Integration Mechanism**:
   - Observation 5 and standard AGP architecture require `find_package(oboe REQUIRED CONFIG)` in CMake. When AGP builds with `buildFeatures.prefab = true` and `com.google.oboe:oboe:1.9.0`, AGP generates the CMake imported target `oboe::oboe`.
   - Because Oboe is built with `c++_shared`, Gradle must configure `-DANDROID_STL=c++_shared` to prevent duplicate libc++ runtime instantiations.
4. **Symbol Isolation and Performance**:
   - Setting `-fvisibility=hidden` and `-fvisibility-inlines-hidden` ensures all private engine classes in `vban` namespace remain internal to `libvban_engine.so`. Only methods explicitly marked `JNIEXPORT` are exposed in the dynamic symbol table.
   - Using `-O3` without `-ffast-math` ensures SIMD optimization without compromising IEEE 754 float validation (`std::isnan`, `std::isinf`) required by the VBAN protocol.
5. **Native Skeleton Design**:
   - Designing `vban_engine.hpp` and `vban_engine.cpp` with a dedicated verification routine `verifyCpp20AndOboeEnvironment()` validates compiler options, endianness, spans, concepts, and Oboe `AudioStreamBuilder` instantiation at startup.
   - Including guarded JNI stubs matching `PROJECT.md` allows the `:app` module to load and link `libvban_engine.so` immediately during Milestone 1 verification without runtime linking errors.

---

## 3. Caveats

1. **Oboe Headers Availability During Standalone CMake**:
   - `app/src/main/cpp/CMakeLists.txt` depends on `find_package(oboe REQUIRED CONFIG)`, which is resolved via AGP Prefab. Attempting to run CMake directly on `app/src/main/cpp/CMakeLists.txt` without Gradle or Prefab tooling will fail to locate `oboeConfig.cmake`. Standalone host testing belongs strictly in `native_tests/` (which does not depend on Oboe or Android OS libraries).
2. **Architecture Support**:
   - Tested specifically on `x86_64` (host) and `arm64-v8a` (target). While `x86_64` and `armeabi-v7a` are supported by NDK 26.1 and Oboe, modern audiophile target devices will run primarily on 64-bit (`arm64-v8a`).

---

## 4. Conclusion

The CMake and C++20 configuration is completely designed, verified against the local NDK 26.1 toolchain, and ready for file creation by the Milestone 1 implementer:

1. **`app/src/main/cpp/CMakeLists.txt`**:
   - Enforces CMake 3.22.1 and ISO C++20.
   - Finds and links `oboe::oboe` via Prefab, along with `log` and `android`.
   - Configures `-O3`, `-Wall`, `-Wextra`, and `-fvisibility=hidden`.
   - Targets `add_library(vban_engine SHARED src/vban_engine.cpp)` with explicit extension placeholders for M2-M4.
2. **`app/src/main/cpp/include/vban_engine.hpp`**:
   - Defines `AudioStreamConfig`, `NativeTelemetry`, and `AudioSampleType` concept.
   - Declares `VbanEngine` singleton with atomic lifecycle and verification API.
3. **`app/src/main/cpp/src/vban_engine.cpp`**:
   - Implements `verifyCpp20AndOboeEnvironment()`, lifecycle management, and full JNI entry points conforming to `PROJECT.md`.

Detailed templates are documented in `survey_report.md`.

---

## 5. Verification Method

To independently verify the configuration:

1. **Inspect Artifacts**:
   - View `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m1_cmake\survey_report.md` for complete file contents.
2. **Target Compilation Test**:
   - Run the NDK Clang ARM64 test command:
     ```powershell
     & "D:\Android\sdk\ndk\26.1.10909125\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android26-clang++.cmd" -std=c++20 -shared -fPIC <source_file> -o libvban_engine.so -llog
     ```
3. **Full Gradle Build Verification** (when implementer writes files):
   - Execute `./gradlew assembleDebug` (or `.\gradlew.bat assembleDebug`).
   - Confirm that CMake configures `vban_engine` and links `oboe::oboe` without error.
   - Invalidation conditions: Gradle build failure during CMake generation, Oboe package not found, or C++20 compiler flags rejected.

---
