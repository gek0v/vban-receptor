#include "vban_converter.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <bit>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace vban {

// ==============================================================================
// Memory Unaligned Load Helpers (Safe against Alignment Faults & Strict Aliasing)
// ==============================================================================
namespace {

inline int16_t loadInt16LE(const uint8_t* p) noexcept {
    int16_t val;
    std::memcpy(&val, p, sizeof(int16_t));
    return val;
}

inline int32_t loadInt32LE(const uint8_t* p) noexcept {
    int32_t val;
    std::memcpy(&val, p, sizeof(int32_t));
    return val;
}

inline float loadFloat32LE(const uint8_t* p) noexcept {
    float val;
    std::memcpy(&val, p, sizeof(float));
    return val;
}

} // anonymous namespace

// ==============================================================================
// Audio Format Dequantizers: Scalar Reference Oracles
// ==============================================================================

void convertInt16ToFloatScalar(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    // Fast path: 2-byte aligned source pointer with 4x unrolling
    if ((reinterpret_cast<uintptr_t>(src) & 0x1) == 0) {
        const auto* __restrict in = reinterpret_cast<const int16_t*>(src);
        size_t i = 0;
        const size_t unrolled = totalSamples & ~size_t{3};
        for (; i < unrolled; i += 4) {
            dst[i + 0] = static_cast<float>(in[i + 0]) * kScaleInt16;
            dst[i + 1] = static_cast<float>(in[i + 1]) * kScaleInt16;
            dst[i + 2] = static_cast<float>(in[i + 2]) * kScaleInt16;
            dst[i + 3] = static_cast<float>(in[i + 3]) * kScaleInt16;
        }
        for (; i < totalSamples; ++i) {
            dst[i] = static_cast<float>(in[i]) * kScaleInt16;
        }
    } else {
        // Safe path for unaligned memory
        for (size_t i = 0; i < totalSamples; ++i) {
            int16_t val = loadInt16LE(src + i * 2);
            dst[i] = static_cast<float>(val) * kScaleInt16;
        }
    }
}

void convertInt24ToFloatScalar(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    size_t i = 0;
    const size_t unrolled = totalSamples & ~size_t{3};
    for (; i < unrolled; i += 4) {
        const size_t o0 = (i + 0) * 3;
        const uint32_t b0_0 = static_cast<uint32_t>(src[o0 + 0]);
        const uint32_t b1_0 = static_cast<uint32_t>(src[o0 + 1]);
        const uint32_t b2_0 = static_cast<uint32_t>(src[o0 + 2]);
        const uint32_t raw0 = b0_0 | (b1_0 << 8) | (b2_0 << 16);
        dst[i + 0] = static_cast<float>(static_cast<int32_t>(raw0 << 8) >> 8) * kScaleInt24;

        const size_t o1 = (i + 1) * 3;
        const uint32_t b0_1 = static_cast<uint32_t>(src[o1 + 0]);
        const uint32_t b1_1 = static_cast<uint32_t>(src[o1 + 1]);
        const uint32_t b2_1 = static_cast<uint32_t>(src[o1 + 2]);
        const uint32_t raw1 = b0_1 | (b1_1 << 8) | (b2_1 << 16);
        dst[i + 1] = static_cast<float>(static_cast<int32_t>(raw1 << 8) >> 8) * kScaleInt24;

        const size_t o2 = (i + 2) * 3;
        const uint32_t b0_2 = static_cast<uint32_t>(src[o2 + 0]);
        const uint32_t b1_2 = static_cast<uint32_t>(src[o2 + 1]);
        const uint32_t b2_2 = static_cast<uint32_t>(src[o2 + 2]);
        const uint32_t raw2 = b0_2 | (b1_2 << 8) | (b2_2 << 16);
        dst[i + 2] = static_cast<float>(static_cast<int32_t>(raw2 << 8) >> 8) * kScaleInt24;

        const size_t o3 = (i + 3) * 3;
        const uint32_t b0_3 = static_cast<uint32_t>(src[o3 + 0]);
        const uint32_t b1_3 = static_cast<uint32_t>(src[o3 + 1]);
        const uint32_t b2_3 = static_cast<uint32_t>(src[o3 + 2]);
        const uint32_t raw3 = b0_3 | (b1_3 << 8) | (b2_3 << 16);
        dst[i + 3] = static_cast<float>(static_cast<int32_t>(raw3 << 8) >> 8) * kScaleInt24;
    }

    for (; i < totalSamples; ++i) {
        const size_t offset = i * 3;
        const uint32_t b0 = static_cast<uint32_t>(src[offset + 0]);
        const uint32_t b1 = static_cast<uint32_t>(src[offset + 1]);
        const uint32_t b2 = static_cast<uint32_t>(src[offset + 2]);

        const uint32_t raw24 = b0 | (b1 << 8) | (b2 << 16);
        const int32_t s32 = static_cast<int32_t>(raw24 << 8) >> 8;
        dst[i] = static_cast<float>(s32) * kScaleInt24;
    }
}

// ==============================================================================
// Audio Format Dequantizers: ARM NEON SIMD Vectorization
// ==============================================================================

#if defined(__ARM_NEON) || defined(__aarch64__)
void convertInt16ToFloatNeon(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    const float32x4_t vscale = vdupq_n_f32(kScaleInt16);
    size_t i = 0;
    const size_t simdSamples = totalSamples & ~size_t{7}; // 8 samples per iteration

    // Check 2-byte alignment for direct vld1q_s16
    if ((reinterpret_cast<uintptr_t>(src) & 0x1) == 0) {
        const auto* __restrict in16 = reinterpret_cast<const int16_t*>(src);
        for (; i < simdSamples; i += 8) {
            // Load 8 int16 samples
            const int16x8_t samples16 = vld1q_s16(in16 + i);

            // Sign-extend to two int32x4_t
            const int32x4_t s32_lo = vmovl_s16(vget_low_s16(samples16));
            const int32x4_t s32_hi = vmovl_s16(vget_high_s16(samples16));

            // Convert integer to float
            float32x4_t f_lo = vcvtq_f32_s32(s32_lo);
            float32x4_t f_hi = vcvtq_f32_s32(s32_hi);

            // Multiply by kScaleInt16
            f_lo = vmulq_f32(f_lo, vscale);
            f_hi = vmulq_f32(f_hi, vscale);

            // Store results
            vst1q_f32(dst + i, f_lo);
            vst1q_f32(dst + i + 4, f_hi);
        }
    } else {
        // Safe path for unaligned memory
        for (; i < simdSamples; i += 8) {
            const int16x8_t samples16 = vreinterpretq_s16_u8(vld1q_u8(src + i * 2));
            const int32x4_t s32_lo = vmovl_s16(vget_low_s16(samples16));
            const int32x4_t s32_hi = vmovl_s16(vget_high_s16(samples16));

            float32x4_t f_lo = vcvtq_f32_s32(s32_lo);
            float32x4_t f_hi = vcvtq_f32_s32(s32_hi);

            f_lo = vmulq_f32(f_lo, vscale);
            f_hi = vmulq_f32(f_hi, vscale);

            vst1q_f32(dst + i, f_lo);
            vst1q_f32(dst + i + 4, f_hi);
        }
    }

    // Branchless scalar cleanup for remaining samples
    for (; i < totalSamples; ++i) {
        int16_t val = loadInt16LE(src + i * 2);
        dst[i] = static_cast<float>(val) * kScaleInt16;
    }
}

void convertInt24ToFloatNeon(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    const float32x4_t vscale = vdupq_n_f32(kScaleInt24);
    size_t i = 0;
    const size_t simdSamples = totalSamples & ~size_t{7}; // 8 samples (24 bytes) per iteration

    for (; i < simdSamples; i += 8) {
        // Load 8 24-bit samples (24 bytes) deinterleaved into 3 vectors of 8 bytes:
        // val[0] = byte 0 (LSB) of 8 samples
        // val[1] = byte 1 (MID) of 8 samples
        // val[2] = byte 2 (MSB/sign) of 8 samples
        const uint8x8x3_t packed = vld3_u8(src + i * 3);

        // Widen each byte channel to 16-bit
        const uint16x8_t b0_16 = vmovl_u8(packed.val[0]);
        const uint16x8_t b1_16 = vmovl_u8(packed.val[1]);
        const uint16x8_t b2_16 = vmovl_u8(packed.val[2]);

        // Widen to 32-bit (low half: samples 0..3, high half: samples 4..7)
        const uint32x4_t b0_lo = vmovl_u16(vget_low_u16(b0_16));
        const uint32x4_t b0_hi = vmovl_u16(vget_high_u16(b0_16));

        const uint32x4_t b1_lo = vmovl_u16(vget_low_u16(b1_16));
        const uint32x4_t b1_hi = vmovl_u16(vget_high_u16(b1_16));

        const uint32x4_t b2_lo = vmovl_u16(vget_low_u16(b2_16));
        const uint32x4_t b2_hi = vmovl_u16(vget_high_u16(b2_16));

        // Shift left to place sign bit at MSB (bit 31):
        // (b2 << 24) | (b1 << 16) | (b0 << 8)
        const uint32x4_t shift_lo = vorrq_u32(
            vshlq_n_u32(b2_lo, 24),
            vorrq_u32(vshlq_n_u32(b1_lo, 16), vshlq_n_u32(b0_lo, 8))
        );
        const uint32x4_t shift_hi = vorrq_u32(
            vshlq_n_u32(b2_hi, 24),
            vorrq_u32(vshlq_n_u32(b1_hi, 16), vshlq_n_u32(b0_hi, 8))
        );

        // Arithmetic right-shift by 8 to sign-extend 24-bit value to 32-bit signed int
        const int32x4_t s32_lo = vshrq_n_s32(vreinterpretq_s32_u32(shift_lo), 8);
        const int32x4_t s32_hi = vshrq_n_s32(vreinterpretq_s32_u32(shift_hi), 8);

        // Convert integer to float
        float32x4_t f_lo = vcvtq_f32_s32(s32_lo);
        float32x4_t f_hi = vcvtq_f32_s32(s32_hi);

        // Multiply by 2^-23 scale factor
        f_lo = vmulq_f32(f_lo, vscale);
        f_hi = vmulq_f32(f_hi, vscale);

        // Store 8 float results
        vst1q_f32(dst + i, f_lo);
        vst1q_f32(dst + i + 4, f_hi);
    }

    // Branchless scalar cleanup for remaining samples
    for (; i < totalSamples; ++i) {
        const size_t offset = i * 3;
        const uint32_t b0 = static_cast<uint32_t>(src[offset + 0]);
        const uint32_t b1 = static_cast<uint32_t>(src[offset + 1]);
        const uint32_t b2 = static_cast<uint32_t>(src[offset + 2]);

        const uint32_t raw24 = b0 | (b1 << 8) | (b2 << 16);
        const int32_t s32 = static_cast<int32_t>(raw24 << 8) >> 8;
        dst[i] = static_cast<float>(s32) * kScaleInt24;
    }
}
#endif

// ==============================================================================
// Audio Format Dequantizers: Public Dispatchers
// ==============================================================================

void convertInt16ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
#if defined(__ARM_NEON) || defined(__aarch64__)
    convertInt16ToFloatNeon(src, dst, totalSamples);
#else
    convertInt16ToFloatScalar(src, dst, totalSamples);
#endif
}

void convertInt24ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
#if defined(__ARM_NEON) || defined(__aarch64__)
    convertInt24ToFloatNeon(src, dst, totalSamples);
#else
    convertInt24ToFloatScalar(src, dst, totalSamples);
#endif
}

void convertInt32ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    // Fast path: 4-byte aligned source pointer
    if ((reinterpret_cast<uintptr_t>(src) & 0x3) == 0) {
        const auto* __restrict in = reinterpret_cast<const int32_t*>(src);
        for (size_t i = 0; i < totalSamples; ++i) {
            dst[i] = static_cast<float>(in[i]) * kScaleInt32;
        }
    } else {
        // Safe path for unaligned memory
        for (size_t i = 0; i < totalSamples; ++i) {
            int32_t val = loadInt32LE(src + i * 4);
            dst[i] = static_cast<float>(val) * kScaleInt32;
        }
    }
}

void convertFloat32Sanitized(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    if ((reinterpret_cast<uintptr_t>(src) & 0x3) == 0) {
        const auto* __restrict in = reinterpret_cast<const float*>(src);
        for (size_t i = 0; i < totalSamples; ++i) {
            const float v = in[i];
            if (!std::isfinite(v)) [[unlikely]] {
                dst[i] = 0.0f;
            } else {
                dst[i] = std::clamp(v, -1.0f, 1.0f);
            }
        }
    } else {
        for (size_t i = 0; i < totalSamples; ++i) {
            const float v = loadFloat32LE(src + i * 4);
            if (!std::isfinite(v)) [[unlikely]] {
                dst[i] = 0.0f;
            } else {
                dst[i] = std::clamp(v, -1.0f, 1.0f);
            }
        }
    }
}

// ==============================================================================
// Unified Dispatcher
// ==============================================================================

bool convertVbanPayloadToFloat(
    uint8_t formatCode,
    const uint8_t* __restrict src,
    float* __restrict dst,
    size_t totalSamples
) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return false;
    }

    const auto format = static_cast<VbanAudioFormat>(formatCode & 0x07);
    switch (format) {
        case VbanAudioFormat::Int16:
            convertInt16ToFloat(src, dst, totalSamples);
            return true;

        case VbanAudioFormat::Int24:
            convertInt24ToFloat(src, dst, totalSamples);
            return true;

        case VbanAudioFormat::Int32:
            convertInt32ToFloat(src, dst, totalSamples);
            return true;

        case VbanAudioFormat::Float32:
            convertFloat32Sanitized(src, dst, totalSamples);
            return true;

        case VbanAudioFormat::Byte8: {
            // 8-bit unsigned integer (0..255) with bias 128
            constexpr float kScaleByte8 = 1.0f / 128.0f;
            for (size_t i = 0; i < totalSamples; ++i) {
                dst[i] = (static_cast<float>(src[i]) - 128.0f) * kScaleByte8;
            }
            return true;
        }

        default:
            // Unsupported or reserved format (Bits10, Bits12, Float64)
            return false;
    }
}

void calculatePeakLevelsStereoScalar(
    const float* __restrict interleaved,
    size_t numFrames,
    float& peakLeft,
    float& peakRight
) noexcept {
    float maxL = 0.0f;
    float maxR = 0.0f;

    if (interleaved && numFrames > 0) {
        size_t f = 0;
        const size_t unrolled = numFrames & ~size_t{3};
        for (; f < unrolled; f += 4) {
            const float l0 = std::abs(interleaved[(f + 0) * 2 + 0]);
            const float r0 = std::abs(interleaved[(f + 0) * 2 + 1]);
            const float l1 = std::abs(interleaved[(f + 1) * 2 + 0]);
            const float r1 = std::abs(interleaved[(f + 1) * 2 + 1]);
            const float l2 = std::abs(interleaved[(f + 2) * 2 + 0]);
            const float r2 = std::abs(interleaved[(f + 2) * 2 + 1]);
            const float l3 = std::abs(interleaved[(f + 3) * 2 + 0]);
            const float r3 = std::abs(interleaved[(f + 3) * 2 + 1]);

            if (l0 > maxL) maxL = l0;
            if (r0 > maxR) maxR = r0;
            if (l1 > maxL) maxL = l1;
            if (r1 > maxR) maxR = r1;
            if (l2 > maxL) maxL = l2;
            if (r2 > maxR) maxR = r2;
            if (l3 > maxL) maxL = l3;
            if (r3 > maxR) maxR = r3;
        }

        for (; f < numFrames; ++f) {
            const float l = std::abs(interleaved[f * 2 + 0]);
            const float r = std::abs(interleaved[f * 2 + 1]);
            if (l > maxL) maxL = l;
            if (r > maxR) maxR = r;
        }
    }

    peakLeft  = std::min(maxL, 1.0f);
    peakRight = std::min(maxR, 1.0f);
}

#if defined(__ARM_NEON) || defined(__aarch64__)
void calculatePeakLevelsStereoNeon(
    const float* __restrict interleaved,
    size_t numFrames,
    float& peakLeft,
    float& peakRight
) noexcept {
    if (!interleaved || numFrames == 0) [[unlikely]] {
        peakLeft = 0.0f;
        peakRight = 0.0f;
        return;
    }

    float maxL = 0.0f;
    float maxR = 0.0f;

    size_t f = 0;
    const size_t simdFrames = numFrames & ~size_t{3}; // 4 frames (8 floats) per iteration

    if (simdFrames > 0) {
        float32x4_t maxL_vec = vdupq_n_f32(0.0f);
        float32x4_t maxR_vec = vdupq_n_f32(0.0f);

        // Process 8 frames per iteration if available
        for (; f + 8 <= numFrames; f += 8) {
            const float32x4x2_t f0 = vld2q_f32(interleaved + f * 2);
            const float32x4x2_t f1 = vld2q_f32(interleaved + (f + 4) * 2);

            const float32x4_t absL0 = vabsq_f32(f0.val[0]);
            const float32x4_t absR0 = vabsq_f32(f0.val[1]);
            const float32x4_t absL1 = vabsq_f32(f1.val[0]);
            const float32x4_t absR1 = vabsq_f32(f1.val[1]);

#if defined(__aarch64__)
            maxL_vec = vmaxnmq_f32(maxL_vec, vmaxnmq_f32(absL0, absL1));
            maxR_vec = vmaxnmq_f32(maxR_vec, vmaxnmq_f32(absR0, absR1));
#else
            maxL_vec = vmaxq_f32(maxL_vec, vmaxq_f32(absL0, absL1));
            maxR_vec = vmaxq_f32(maxR_vec, vmaxq_f32(absR0, absR1));
#endif
        }

        // Remaining 4-frame chunk
        for (; f < simdFrames; f += 4) {
            const float32x4x2_t f0 = vld2q_f32(interleaved + f * 2);
            const float32x4_t absL0 = vabsq_f32(f0.val[0]);
            const float32x4_t absR0 = vabsq_f32(f0.val[1]);

#if defined(__aarch64__)
            maxL_vec = vmaxnmq_f32(maxL_vec, absL0);
            maxR_vec = vmaxnmq_f32(maxR_vec, absR0);
#else
            maxL_vec = vmaxq_f32(maxL_vec, absL0);
            maxR_vec = vmaxq_f32(maxR_vec, absR0);
#endif
        }

#if defined(__aarch64__)
        maxL = vmaxnmvq_f32(maxL_vec);
        maxR = vmaxnmvq_f32(maxR_vec);
#else
        float32x2_t maxL_pair = vpmax_f32(vget_low_f32(maxL_vec), vget_high_f32(maxL_vec));
        maxL_pair = vpmax_f32(maxL_pair, maxL_pair);
        maxL = vget_lane_f32(maxL_pair, 0);

        float32x2_t maxR_pair = vpmax_f32(vget_low_f32(maxR_vec), vget_high_f32(maxR_vec));
        maxR_pair = vpmax_f32(maxR_pair, maxR_pair);
        maxR = vget_lane_f32(maxR_pair, 0);
#endif
    }

    // Branchless scalar cleanup for remaining frames
    for (; f < numFrames; ++f) {
        const float l = std::abs(interleaved[f * 2 + 0]);
        const float r = std::abs(interleaved[f * 2 + 1]);
        if (l > maxL) maxL = l;
        if (r > maxR) maxR = r;
    }

    peakLeft  = std::min(maxL, 1.0f);
    peakRight = std::min(maxR, 1.0f);
}
#endif

void calculatePeakLevelsStereo(
    const float* __restrict interleaved,
    size_t numFrames,
    float& peakLeft,
    float& peakRight
) noexcept {
#if defined(__ARM_NEON) || defined(__aarch64__)
    calculatePeakLevelsStereoNeon(interleaved, numFrames, peakLeft, peakRight);
#else
    calculatePeakLevelsStereoScalar(interleaved, numFrames, peakLeft, peakRight);
#endif
}

} // namespace vban
