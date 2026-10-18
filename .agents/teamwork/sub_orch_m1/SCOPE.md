# Scope: Milestone 1 - Build Scaffolding & Native Test Harness

## Objective
Establish the complete Android Gradle and Native C++ build harness for VBAN Receptor, with NDK 26.1, CMake 3.22+, Oboe 1.9.0 Prefab integration, Jetpack Compose, and a standalone host/NDK C++ unit test runner.

## Architecture
- Root Project: `settings.gradle.kts`, `build.gradle.kts`, `gradle.properties`, `gradlew.bat`
- App Module: `app/build.gradle.kts` with:
  - compileSdk = 34, minSdk = 26, targetSdk = 34
  - `buildFeatures { compose = true; prefab = true }`
  - Compose compiler & dependencies (BOM 2024.02.00 or compatible, material3, ui, foundation)
  - `externalNativeBuild.cmake` targeting `app/src/main/cpp/CMakeLists.txt`, `-DANDROID_STL=c++_shared`, `-std=c++20`
  - Dependencies: `com.google.oboe:oboe:1.9.0` (Prefab AAR)
- Native Build: `app/src/main/cpp/CMakeLists.txt`
  - C++20 standard required
  - `find_package(oboe REQUIRED CONFIG)`
  - Link `oboe::oboe`, `android`, `log`
- Standalone Native Tests: `native_tests/CMakeLists.txt`
  - Standalone C++20 test runner that can be built and executed directly via CMake / NDK Clang on the host system without requiring an Android device or emulator.

## Feature Inventory Assignment
- Feature 1: NDK C++20 & CMake Scaffolding
- Feature 2: Oboe 1.9.0 Prefab Linkage
- Feature 3: Standalone Native Test Harness

## Acceptance Criteria
- [ ] `./gradlew assembleDebug` (or `gradlew.bat assembleDebug`) succeeds.
- [ ] CMake configures and links Oboe 1.9.0 via Prefab without errors.
- [ ] Standalone native test harness compiles and runs cleanly.
