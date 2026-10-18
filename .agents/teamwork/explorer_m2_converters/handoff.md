# Milestone 2 Handoff Report: Audio Dequantizers & Normalization Architecture

**Sender:** `explorer_m2_converters`  
**Recipient:** `9bb0c473-fb0a-499a-bbf2-a430786d0058` (Sub-Orchestrator M2)  
**Timestamp:** 2026-10-18T14:49:00Z  
**Type:** Hard Handoff (Investigation & Code Templates Complete)

---

## 1. Observation

1. **Protocol Specifications & Scale Requirements:**
   - From `D:\Workspaces\vban-receptor\.agents\teamwork\spec_miner_vban\survey_report.md` lines 280-363:
     - 16-bit PCM signed LE: requires normalizer scale factor `1.0f / 32768.0f`.
     - 24-bit packed signed LE: 3 contiguous bytes per sample, requiring sign-extension from 24 bits to 32 bits and normalizer scale factor `1.0f / 8388608.0f`.
     - 32-bit PCM signed LE: requires normalizer scale factor `1.0f / 2147483648.0f`.
     - Float32 IEEE 754: requires replacing `NaN` and `\pm \infty` with `0.0f`, followed by clamping to `[-1.0f, +1.0f]`.
2. **Interleaving & Channel Structure:**
   - From `D:\Workspaces\vban-receptor\.agents\teamwork\spec_miner_vban\survey_report.md` lines 261-279:
     - Samples in VBAN payload are interleaved across channels sequentially per frame: $[L_0, R_0, L_1, R_1, \dots]$.
     - Multi-channel audio requires planar deinterleaving for discrete DSP and per-channel peak meter calculation for Feature 41 (VU Meter canvas).
3. **Existing Native Test Framework:**
   - From `D:\Workspaces\vban-receptor\native_tests\test_main.cpp` lines 167-218 and `native_tests/CMakeLists.txt` lines 23-25:
     - CMake automatically detects and links `native_tests/test_vban_converter.cpp` and `app/src/main/cpp/src/vban_converter.cpp` if they exist.
     - Standalone test suite builds with MSVC/Clang under C++20 standard (`-std=c++20`).
4. **Target File Locations:**
   - `app/src/main/cpp/include/vban_converter.hpp` (Header)
   - `app/src/main/cpp/src/vban_converter.cpp` (Implementation)
   - `native_tests/test_vban_converter.cpp` (Unit tests)

---

## 2. Logic Chain

1. **Power-of-Two Divisors Prevent Numerical Clipping & Mantissa Truncation:**
   - As observed in (1), two's complement integer ranges are asymmetric ($[-2^{N-1}, +2^{N-1}-1]$).
   - Dividing by $2^{N-1}$ maps the extreme negative sample ($-32768$, $-8388608$, $-2147483648$) to exactly $-1.0\text{f}$.
   - Because $2^{N-1}$ is an exact power of two in IEEE 754, multiplication by $2^{-(N-1)}$ merely subtracts $(N-1)$ from the floating point exponent, preserving 100% of the sample's significant bits without rounding error.
2. **Branchless Sign Extension for 24-bit Packed Audio:**
   - 24-bit audio in VBAN is packed as 3 bytes $[b_0, b_1, b_2]$ with bit 7 of $b_2$ as the sign bit.
   - Assembling into `uint32_t raw = b0 | (b1 << 8) | (b2 << 16)`, then executing `(static_cast<int32_t>(raw << 8)) >> 8` performs branchless arithmetic shift sign-extension guaranteed by C++20 ([expr.shift]).
   - This executes in two instructions (`lsl` + `asr` on ARM64; `shl` + `sar` on x86) with zero branches, eliminating branch mispredictions in high sample rate streams (up to 768 kHz).
3. **Float32 Network Sanitization Protects Downstream DSP and DAC:**
   - Malformed packets or network corruption can deliver `NaN` or `\pm \infty`.
   - Feeding `NaN` into IIR filters or Oboe/AAudio corrupts state registers permanently and risks audio thread crashes.
   - Using `!std::isfinite(sample)` efficiently catches all `NaN` and `Inf` variants, safely substituting `0.0f`, followed by `std::clamp(sample, -1.0f, +1.0f)`.
4. **Channel Layout & Telemetry Support:**
   - As observed in (2), audio streams require both interleaved (Oboe playback) and planar/channel-pointer (multi-channel processing) layouts.
   - Providing bidirectional `interleave` and `deinterleave` for planar contiguous buffers, channel pointer arrays, and optimized stereo paths covers all audio pipeline requirements.
   - Providing `calculatePeakLevelsStereo` enables lock-free peak amplitude tracking for the Jetpack Compose VU meter.

---

## 3. Caveats

- **Byte8 Format:** VBAN defines `VBAN_DATATYPE_BYTE8` (offset 128 bias). While secondary to audiophile bit depths, support is included in the unified dispatcher `convertVbanPayloadToFloat`.
- **Extended Formats (10-bit, 12-bit, Float64):** Defined in the VBAN specification for specialized hardware, but exceedingly rare in real-world broadcast audio. The dispatcher rejects them gracefully (`return false`) to protect engine integrity.
- **Strict Aliasing & Unaligned Reads:** If source payload pointers are unaligned, the implementation utilizes `loadInt16LE`, `loadInt32LE`, and `loadFloat32LE` (via `std::memcpy`), which modern Clang/GCC optimize into single unaligned load instructions on ARM64 and x86_64 without undefined behavior.

---

## 4. Conclusion

Complete, production-ready C++20 code templates have been generated and documented in `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m2_converters\survey_report.md`:
1. `app/src/main/cpp/include/vban_converter.hpp` (Section 5.1 of report):
   - Single-sample primitives (`dequantizeInt16`, `dequantizeInt24Packed`, `dequantizeInt32`, `sanitizeFloat32`).
   - Raw pointer zero-allocation functions (`convertInt16ToFloat`, `convertInt24ToFloat`, `convertInt32ToFloat`, `convertFloat32Sanitized`).
   - C++20 `std::span` safe overloads.
   - Unified format dispatcher `convertVbanPayloadToFloat`.
   - Channel transformation functions (`deinterleave`, `interleave`, `deinterleaveStereo`, `interleaveStereo`).
   - Telemetry peak detector (`calculatePeakLevelsStereo`).
2. `app/src/main/cpp/src/vban_converter.cpp` (Section 5.2 of report):
   - High-performance, branchless, zero-allocation implementation with alignment fast-paths.
3. `native_tests/test_vban_converter.cpp` (Section 5.3 of report):
   - Comprehensive test suite with 14 test cases covering zero, positive/negative boundaries, sign flips, negative packed 24-bit values, quiet/signaling NaN, infinities, multichannel planar roundtrip, stereo interleaving roundtrip, peak telemetry, and unified dispatch.

The Builder agent can directly copy these templates into their respective locations.

---

## 5. Verification Method

Once files are copied into the repository:
1. **Host Native Test Execution:**
   ```powershell
   cmake -B native_tests/build -S native_tests
   cmake --build native_tests/build
   .\native_tests\build\vban_native_tests.exe
   ```
   **Expected result:** All converter test cases pass with 0 failures.
2. **Targeted Filter Test:**
   ```powershell
   .\native_tests\build\vban_native_tests.exe --filter=converter
   ```
3. **Android NDK Compilation:**
   ```powershell
   .\gradlew.bat assembleDebug
   ```
   **Expected result:** `BUILD SUCCESSFUL`, `libvban_engine.so` compiled with C++20 and linked with Oboe 1.9.0.
