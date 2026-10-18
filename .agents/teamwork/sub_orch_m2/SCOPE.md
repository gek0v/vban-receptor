# Scope: Milestone 2 - Native VBAN Core (Protocol, Formats & SPSC Ring Buffer)

## Objective
Implement the complete native C++20 VBAN Protocol core, high-fidelity audio dequantizers, and lock-free SPSC circular ring buffer into `app/src/main/cpp/`, and integrate comprehensive unit tests into `native_tests/`.

## Architecture & Code Layout
- `app/src/main/cpp/include/vban_protocol.hpp`:
  - Packed 28-byte `VBanHeader` struct matching specification.
  - Constants for FourCC (`0x4E414256`), subprotocols, codecs, data types.
  - Table of 21 official sample rates + 768 kHz extension (`VBAN_SR_TABLE`).
  - Helper functions for sample size, rate lookup, payload byte length calculation.
- `app/src/main/cpp/include/vban_parser.hpp` and `src/vban_parser.cpp`:
  - 10-step strict packet validation pipeline.
  - Header decoding, stream name matching (null-terminated/padded trimming), payload length validation.
- `app/src/main/cpp/include/vban_converter.hpp` and `src/vban_converter.cpp`:
  - Conversion routines to Float32 `[-1.0f, +1.0f]`:
    - `convertInt16ToFloat(const uint8_t* src, float* dst, size_t totalSamples)`
    - `convertInt24ToFloat(const uint8_t* src, float* dst, size_t totalSamples)` (3-byte packed signed LE with sign extension)
    - `convertInt32ToFloat(const uint8_t* src, float* dst, size_t totalSamples)`
    - `convertFloat32Sanitized(const uint8_t* src, float* dst, size_t totalSamples)` (clamping & NaN/Inf sanitization)
  - Channel deinterleaving/interleaving utilities.
- `app/src/main/cpp/include/spsc_ring_buffer.hpp`:
  - `SpscRingBuffer<float, 262144>`:
    - 64-byte cache line alignment (`alignas(64)`) on read and write counters to prevent false sharing.
    - Capacity: $2^{18} = 262,144$ frames (341.3 ms at 768 kHz, 2 MB RAM).
    - Monotonic `uint64_t` indices with `std::memory_order_release` and `std::memory_order_acquire`.
    - Zero allocations, zero mutexes, zero locks during streaming.
    - Non-blocking `write(const float* data, size_t frames) -> size_t`.
    - Non-blocking `read(float* dst, size_t frames) -> size_t`.
    - `availableRead() const noexcept` and `availableWrite() const noexcept`.
- `native_tests/`:
  - Modular unit tests: `test_vban_protocol.cpp`, `test_vban_converter.cpp`, `test_spsc_buffer.cpp`.
  - Linked into `vban_native_tests`.

## Feature Inventory Assignment
- Features 4 to 19 in `PROJECT.md § Feature Inventory`.

## Acceptance Criteria
- [ ] Header layout verified exactly 28 bytes packed without padding.
- [ ] 21 official sample rates and 768 kHz extension accurately decoded.
- [ ] Audio dequantizers verified with exact mathematical boundaries (int16 min/max, int24 packed sign bit extension min/max, int32 min/max, float32 NaN/Inf replacement).
- [ ] Multi-threaded SPSC circular buffer stress test passes under heavy concurrent load without deadlocks or corruption.
- [ ] Standalone native tests pass 100%.
- [ ] `.\gradlew.bat assembleDebug` succeeds, compiling all M2 native code into `libvban_engine.so`.
