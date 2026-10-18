#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <concepts>
#include <cmath>
#include <algorithm>
#include <bit>
#include <cstring>

namespace vban {

// ==============================================================================
// 1. Exact Mathematical Normalization Scale Constants
// ==============================================================================
inline constexpr float kScaleInt16 = 1.0f / 32768.0f;       // 2^-15 = 3.0517578125e-05f
inline constexpr float kScaleInt24 = 1.0f / 8388608.0f;     // 2^-23 = 1.1920928955078125e-07f
inline constexpr float kScaleInt32 = 1.0f / 2147483648.0f; // 2^-31 = 4.656612873077393e-10f

// ==============================================================================
// 2. VBAN Protocol Audio Data Format Identifiers (Matching format_bit & 0x07)
// ==============================================================================
enum class VbanAudioFormat : uint8_t {
    Byte8   = 0x00, // 8-bit unsigned integer (offset 128 bias)
    Int16   = 0x01, // 16-bit signed integer LE
    Int24   = 0x02, // 24-bit packed signed integer LE (3 bytes)
    Int32   = 0x03, // 32-bit signed integer LE
    Float32 = 0x04, // 32-bit IEEE 754 single-precision float LE
    Float64 = 0x05, // 64-bit IEEE 754 double-precision float LE
    Bits12  = 0x06, // 12-bit signed integer in 16-bit container
    Bits10  = 0x07  // 10-bit signed integer in 16-bit container
};

// ==============================================================================
// 3. Single-Sample Normalization Primitives (Inline & Constexpr-Friendly)
// ==============================================================================

/**
 * Normalizes a 16-bit signed integer to Float32 [-1.0f, +1.0f].
 */
[[nodiscard]] inline constexpr float dequantizeInt16(int16_t sample) noexcept {
    return static_cast<float>(sample) * kScaleInt16;
}

/**
 * Unpacks and normalizes a 24-bit packed signed integer (3 bytes LE) to Float32 [-1.0f, +1.0f].
 * Uses branchless arithmetic sign extension.
 */
[[nodiscard]] inline constexpr float dequantizeInt24Packed(uint8_t b0, uint8_t b1, uint8_t b2) noexcept {
    uint32_t raw24 = static_cast<uint32_t>(b0) |
                    (static_cast<uint32_t>(b1) << 8) |
                    (static_cast<uint32_t>(b2) << 16);
    int32_t s32 = static_cast<int32_t>(raw24 << 8) >> 8;
    return static_cast<float>(s32) * kScaleInt24;
}

/**
 * Unpacks and normalizes a 24-bit packed sample from a 3-byte array pointer.
 */
[[nodiscard]] inline constexpr float dequantizeInt24Packed(const uint8_t bytes[3]) noexcept {
    return dequantizeInt24Packed(bytes[0], bytes[1], bytes[2]);
}

/**
 * Normalizes a 32-bit signed integer to Float32 [-1.0f, +1.0f].
 */
[[nodiscard]] inline constexpr float dequantizeInt32(int32_t sample) noexcept {
    return static_cast<float>(sample) * kScaleInt32;
}

/**
 * Sanitizes an IEEE 754 32-bit float:
 * - Replaces NaN, +Inf, and -Inf with 0.0f
 * - Clamps out-of-range values to [-1.0f, +1.0f]
 */
[[nodiscard]] inline float sanitizeFloat32(float sample) noexcept {
    if (!std::isfinite(sample)) [[unlikely]] {
        return 0.0f;
    }
    return std::clamp(sample, -1.0f, 1.0f);
}

// ==============================================================================
// 4. Zero-Allocation Buffer Conversions (Raw Pointer Interface with Restrict)
// ==============================================================================

/**
 * Converts PCM 16-bit signed LE bytes to normalized Float32 [-1.0f, +1.0f].
 * Dispatches to ARM NEON SIMD vectorization on supported platforms,
 * or optimized scalar fallback on x86/other architectures.
 * @param src Pointer to raw 16-bit PCM bytes (requires at least totalSamples * 2 bytes).
 * @param dst Pointer to destination float buffer (requires at least totalSamples floats).
 * @param totalSamples Total number of samples (channels * frames).
 */
void convertInt16ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;

/**
 * Converts PCM 24-bit packed signed LE bytes (3 bytes per sample) to Float32 [-1.0f, +1.0f].
 * Dispatches to ARM NEON SIMD vectorization on supported platforms,
 * or optimized scalar fallback on x86/other architectures.
 * @param src Pointer to raw 24-bit packed bytes (requires at least totalSamples * 3 bytes).
 * @param dst Pointer to destination float buffer (requires at least totalSamples floats).
 * @param totalSamples Total number of samples (channels * frames).
 */
void convertInt24ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;

/**
 * Converts PCM 32-bit signed LE bytes to normalized Float32 [-1.0f, +1.0f].
 * @param src Pointer to raw 32-bit PCM bytes (requires at least totalSamples * 4 bytes).
 * @param dst Pointer to destination float buffer (requires at least totalSamples floats).
 * @param totalSamples Total number of samples (channels * frames).
 */
void convertInt32ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;

/**
 * Reads IEEE 754 32-bit float bytes and performs sanitization (NaN/Inf to 0.0f, clamping to [-1.0f, +1.0f]).
 * @param src Pointer to raw 32-bit float bytes (requires at least totalSamples * 4 bytes).
 * @param dst Pointer to destination float buffer (requires at least totalSamples floats).
 * @param totalSamples Total number of samples (channels * frames).
 */
void convertFloat32Sanitized(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;

/**
 * Scalar reference oracle implementations for bit-exactness verification and benchmarking.
 */
void convertInt16ToFloatScalar(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;
void convertInt24ToFloatScalar(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;
void calculatePeakLevelsStereoScalar(
    const float* __restrict interleaved,
    size_t numFrames,
    float& peakLeft,
    float& peakRight
) noexcept;

#if defined(__ARM_NEON) || defined(__aarch64__)
/**
 * ARM NEON SIMD vectorized implementations.
 */
void convertInt16ToFloatNeon(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;
void convertInt24ToFloatNeon(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept;
void calculatePeakLevelsStereoNeon(
    const float* __restrict interleaved,
    size_t numFrames,
    float& peakLeft,
    float& peakRight
) noexcept;
#endif

// ==============================================================================
// 5. C++20 std::span Safe Overloads
// ==============================================================================

inline void convertInt16ToFloat(std::span<const uint8_t> src, std::span<float> dst) noexcept {
    const size_t totalSamples = std::min(src.size() / 2, dst.size());
    convertInt16ToFloat(src.data(), dst.data(), totalSamples);
}

inline void convertInt24ToFloat(std::span<const uint8_t> src, std::span<float> dst) noexcept {
    const size_t totalSamples = std::min(src.size() / 3, dst.size());
    convertInt24ToFloat(src.data(), dst.data(), totalSamples);
}

inline void convertInt32ToFloat(std::span<const uint8_t> src, std::span<float> dst) noexcept {
    const size_t totalSamples = std::min(src.size() / 4, dst.size());
    convertInt32ToFloat(src.data(), dst.data(), totalSamples);
}

inline void convertFloat32Sanitized(std::span<const uint8_t> src, std::span<float> dst) noexcept {
    const size_t totalSamples = std::min(src.size() / 4, dst.size());
    convertFloat32Sanitized(src.data(), dst.data(), totalSamples);
}

// ==============================================================================
// 6. Unified VBAN Payload Dispatcher
// ==============================================================================

/**
 * Dispatches conversion based on VBAN format code (format_bit & 0x07).
 * @param formatCode Bits 0..2 of format_bit.
 * @param src Pointer to raw payload bytes.
 * @param dst Destination buffer for interleaved Float32 samples.
 * @param totalSamples Total number of samples (samplesPerFrame * channelCount).
 * @return true if conversion succeeded; false if unsupported format or null pointers.
 */
bool convertVbanPayloadToFloat(
    uint8_t formatCode,
    const uint8_t* __restrict src,
    float* __restrict dst,
    size_t totalSamples
) noexcept;

inline bool convertVbanPayloadToFloat(
    uint8_t formatCode,
    std::span<const uint8_t> src,
    std::span<float> dst
) noexcept {
    return convertVbanPayloadToFloat(formatCode, src.data(), dst.data(), dst.size());
}

// ==============================================================================
// 7. Channel Interleaving / Deinterleaving Utilities
// ==============================================================================

/**
 * Deinterleaves audio frames from interleaved format to contiguous planar format.
 * Planar layout: Channel 0 (numFrames), followed by Channel 1 (numFrames), etc.
 */
void deinterleave(
    const float* __restrict interleaved,
    float* __restrict planar,
    size_t numFrames,
    size_t numChannels
) noexcept;

/**
 * Interleaves audio frames from contiguous planar format to interleaved format.
 */
void interleave(
    const float* __restrict planar,
    float* __restrict interleaved,
    size_t numFrames,
    size_t numChannels
) noexcept;

/**
 * Deinterleaves audio frames from interleaved format to an array of channel buffers.
 * @param channelBuffers Array of pointers, each pointing to a buffer of at least numFrames floats.
 */
void deinterleave(
    const float* __restrict interleaved,
    float* const* __restrict channelBuffers,
    size_t numFrames,
    size_t numChannels
) noexcept;

/**
 * Interleaves audio frames from an array of channel buffers to interleaved format.
 */
void interleave(
    const float* const* __restrict channelBuffers,
    float* __restrict interleaved,
    size_t numFrames,
    size_t numChannels
) noexcept;

/**
 * Optimized stereo deinterleaving for 2-channel audio.
 */
void deinterleaveStereo(
    const float* __restrict interleaved,
    float* __restrict left,
    float* __restrict right,
    size_t numFrames
) noexcept;

/**
 * Optimized stereo interleaving for 2-channel audio.
 */
void interleaveStereo(
    const float* __restrict left,
    const float* __restrict right,
    float* __restrict interleaved,
    size_t numFrames
) noexcept;

// ==============================================================================
// 8. Telemetry Helper: Stereo Peak Level Detection (For VU Meters)
// ==============================================================================

/**
 * Calculates peak absolute amplitude for left and right channels across interleaved stereo frames.
 * Returns peaks in the range [0.0f, 1.0f].
 */
void calculatePeakLevelsStereo(
    const float* __restrict interleaved,
    size_t numFrames,
    float& peakLeft,
    float& peakRight
) noexcept;

} // namespace vban
