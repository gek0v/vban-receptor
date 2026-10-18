#include "vban_converter.hpp"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <bit>

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
// Audio Format Dequantizers
// ==============================================================================

void convertInt16ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    // Fast path: 2-byte aligned source pointer
    if ((reinterpret_cast<uintptr_t>(src) & 0x1) == 0) {
        const auto* __restrict in = reinterpret_cast<const int16_t*>(src);
        for (size_t i = 0; i < totalSamples; ++i) {
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

void convertInt24ToFloat(const uint8_t* __restrict src, float* __restrict dst, size_t totalSamples) noexcept {
    if (!src || !dst || totalSamples == 0) [[unlikely]] {
        return;
    }

    for (size_t i = 0; i < totalSamples; ++i) {
        const size_t offset = i * 3;
        const uint32_t b0 = static_cast<uint32_t>(src[offset + 0]);
        const uint32_t b1 = static_cast<uint32_t>(src[offset + 1]);
        const uint32_t b2 = static_cast<uint32_t>(src[offset + 2]);

        const uint32_t raw24 = b0 | (b1 << 8) | (b2 << 16);
        // Branchless arithmetic sign extension: shift left by 8 to place sign bit at MSB (bit 31),
        // then arithmetic right shift by 8 to replicate sign bit across bits 31..24.
        const int32_t s32 = static_cast<int32_t>(raw24 << 8) >> 8;
        dst[i] = static_cast<float>(s32) * kScaleInt24;
    }
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

// ==============================================================================
// Channel Interleaving / Deinterleaving
// ==============================================================================

void deinterleave(
    const float* __restrict interleaved,
    float* __restrict planar,
    size_t numFrames,
    size_t numChannels
) noexcept {
    if (!interleaved || !planar || numFrames == 0 || numChannels == 0) [[unlikely]] {
        return;
    }

    if (numChannels == 1) {
        std::memcpy(planar, interleaved, numFrames * sizeof(float));
        return;
    }

    for (size_t ch = 0; ch < numChannels; ++ch) {
        float* dstChan = planar + ch * numFrames;
        for (size_t f = 0; f < numFrames; ++f) {
            dstChan[f] = interleaved[f * numChannels + ch];
        }
    }
}

void interleave(
    const float* __restrict planar,
    float* __restrict interleaved,
    size_t numFrames,
    size_t numChannels
) noexcept {
    if (!planar || !interleaved || numFrames == 0 || numChannels == 0) [[unlikely]] {
        return;
    }

    if (numChannels == 1) {
        std::memcpy(interleaved, planar, numFrames * sizeof(float));
        return;
    }

    for (size_t ch = 0; ch < numChannels; ++ch) {
        const float* srcChan = planar + ch * numFrames;
        for (size_t f = 0; f < numFrames; ++f) {
            interleaved[f * numChannels + ch] = srcChan[f];
        }
    }
}

void deinterleave(
    const float* __restrict interleaved,
    float* const* __restrict channelBuffers,
    size_t numFrames,
    size_t numChannels
) noexcept {
    if (!interleaved || !channelBuffers || numFrames == 0 || numChannels == 0) [[unlikely]] {
        return;
    }

    for (size_t ch = 0; ch < numChannels; ++ch) {
        float* dstChan = channelBuffers[ch];
        if (!dstChan) continue;
        for (size_t f = 0; f < numFrames; ++f) {
            dstChan[f] = interleaved[f * numChannels + ch];
        }
    }
}

void interleave(
    const float* const* __restrict channelBuffers,
    float* __restrict interleaved,
    size_t numFrames,
    size_t numChannels
) noexcept {
    if (!channelBuffers || !interleaved || numFrames == 0 || numChannels == 0) [[unlikely]] {
        return;
    }

    for (size_t ch = 0; ch < numChannels; ++ch) {
        const float* srcChan = channelBuffers[ch];
        if (!srcChan) continue;
        for (size_t f = 0; f < numFrames; ++f) {
            interleaved[f * numChannels + ch] = srcChan[f];
        }
    }
}

void deinterleaveStereo(
    const float* __restrict interleaved,
    float* __restrict left,
    float* __restrict right,
    size_t numFrames
) noexcept {
    if (!interleaved || !left || !right || numFrames == 0) [[unlikely]] {
        return;
    }

    for (size_t f = 0; f < numFrames; ++f) {
        left[f]  = interleaved[f * 2 + 0];
        right[f] = interleaved[f * 2 + 1];
    }
}

void interleaveStereo(
    const float* __restrict left,
    const float* __restrict right,
    float* __restrict interleaved,
    size_t numFrames
) noexcept {
    if (!left || !right || !interleaved || numFrames == 0) [[unlikely]] {
        return;
    }

    for (size_t f = 0; f < numFrames; ++f) {
        interleaved[f * 2 + 0] = left[f];
        interleaved[f * 2 + 1] = right[f];
    }
}

void calculatePeakLevelsStereo(
    const float* __restrict interleaved,
    size_t numFrames,
    float& peakLeft,
    float& peakRight
) noexcept {
    float maxL = 0.0f;
    float maxR = 0.0f;

    if (interleaved && numFrames > 0) {
        for (size_t f = 0; f < numFrames; ++f) {
            const float l = std::abs(interleaved[f * 2 + 0]);
            const float r = std::abs(interleaved[f * 2 + 1]);
            if (l > maxL) maxL = l;
            if (r > maxR) maxR = r;
        }
    }

    peakLeft  = std::min(maxL, 1.0f);
    peakRight = std::min(maxR, 1.0f);
}

} // namespace vban
