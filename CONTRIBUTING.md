# Contributing to VBAN Receptor for Android

Thank you for your interest in contributing to **VBAN Receptor for Android**! We welcome community contributions, bug reports, architectural improvements, and feature proposals.

---

## Code of Conduct

We are committed to providing a welcoming, inclusive, and harassment-free environment for everyone. Please be respectful, constructive, and considerate in all communications.

---

## How Can I Contribute?

### 1. Reporting Bugs
Before filing an issue, please:
- Search existing issues to ensure the bug has not already been reported.
- Verify whether the issue persists on the latest `master` branch.
- When opening an issue, use the **Bug Report** template and provide:
  - Android device model and Android OS version.
  - VBAN transmitter details (e.g. Voicemeeter version, sample rate, channels, bit depth).
  - Network configuration (Wi-Fi 2.4/5GHz, Ethernet).
  - Audio output device (Built-in, 3.5mm, USB DAC model).
  - Clear reproduction steps and logcat logs (if applicable).

### 2. Suggesting Enhancements
Feature requests are tracked as GitHub issues. When suggesting improvements, please explain:
- The problem or use-case you are trying to solve.
- How the suggested feature should behave.
- Any alternative approaches or tradeoffs considered.

### 3. Pull Requests
We welcome pull requests for bug fixes, performance optimizations, and new features!

#### PR Guidelines:
1. **Fork and Branch**: Fork the repo and create your branch from `master` with a descriptive name (e.g., `feature/opus-codec-support` or `fix/spsc-overrun-handling`).
2. **Coding Standards**:
   - **Kotlin / Compose**: Follow the official Android Kotlin style guide. Maintain Compose immutable state practices and avoid unnecessary recompositions.
   - **C++20**: Follow modern C++20 conventions. **Strict Real-Time Rule**: Never allocate memory (`malloc`, `new`, STL containers that resize), acquire blocking mutexes, or make OS system calls inside the real-time audio callback (`onAudioReady`) or SPSC ring buffer operations.
   - **Python**: Follow PEP 8 for test scripts and simulation fixtures.
3. **Commit Messages**: Use [Conventional Commits](https://www.conventionalcommits.org/) (e.g., `feat: ...`, `fix: ...`, `docs: ...`, `perf: ...`, `test: ...`).
4. **Verification**: All PRs must pass the test suite:
   ```bash
   # 1. Native C++ tests
   cmake -B build_native_test -S native_tests
   cmake --build build_native_test --config Release
   ctest --test-dir build_native_test -C Release --output-on-failure

   # 2. Python E2E verification suite (84 tests)
   python e2e_tests/run_tests.py

   # 3. Android JVM Unit Tests & Build
   ./gradlew testDebugUnitTest assembleDebug
   ```

---

## Development Setup

1. **Prerequisites**:
   - JDK 17 (Eclipse Adoptium or OpenJDK).
   - Android Studio Iguana+ with Android SDK 34 and NDK `26.1.10909125`.
   - CMake 3.22.1+.
   - Python 3.10+.
2. **Importing into Android Studio**:
   - Open Android Studio -> Open -> Select `vban-receptor` folder.
   - Wait for Gradle sync to complete. Prefab will automatically download and link Google Oboe 1.9.0.
   - Connect an Android device with Developer Options & USB Debugging enabled.
   - Run the app via `Run 'app'`.

---

Thank you for helping make VBAN Receptor the best audiophile network audio receptor for Android!
