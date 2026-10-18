#include "test_framework.hpp"
#include "vban_converter.hpp"

#include <vector>
#include <cmath>
#include <limits>
#include <cstring>

using namespace vban;

// ==============================================================================
// 1. PCM 16-Bit Normalization & Boundary Tests
// ==============================================================================

TEST_CASE("converter_int16_exact_boundaries") {
    // 1. Zero
    EXPECT_NEAR(dequantizeInt16(0), 0.0f, 1e-7f);

    // 2. Full-scale minimum (-32768) -> exactly -1.0f
    EXPECT_NEAR(dequantizeInt16(-32768), -1.0f, 1e-7f);

    // 3. Full-scale maximum (+32767) -> 32767 / 32768
    constexpr float kMaxInt16Float = 32767.0f / 32768.0f;
    EXPECT_NEAR(dequantizeInt16(32767), kMaxInt16Float, 1e-7f);
    ASSERT_LT(dequantizeInt16(32767), 1.0f);

    // 4. Positive and negative unit steps
    EXPECT_NEAR(dequantizeInt16(1), kScaleInt16, 1e-9f);
    EXPECT_NEAR(dequantizeInt16(-1), -kScaleInt16, 1e-9f);

    // 5. Half-scale points
    EXPECT_NEAR(dequantizeInt16(16384), 0.5f, 1e-7f);
    EXPECT_NEAR(dequantizeInt16(-16384), -0.5f, 1e-7f);
}

TEST_CASE("converter_int16_buffer_conversion") {
    const std::vector<int16_t> raw = {
        0, 1, -1, 32767, -32768, 16384, -16384, 8192
    };
    std::vector<float> dst(raw.size(), 0.0f);

    convertInt16ToFloat(
        reinterpret_cast<const uint8_t*>(raw.data()),
        dst.data(),
        raw.size()
    );

    EXPECT_NEAR(dst[0], 0.0f, 1e-7f);
    EXPECT_NEAR(dst[1], kScaleInt16, 1e-7f);
    EXPECT_NEAR(dst[2], -kScaleInt16, 1e-7f);
    EXPECT_NEAR(dst[3], 32767.0f / 32768.0f, 1e-7f);
    EXPECT_NEAR(dst[4], -1.0f, 1e-7f);
    EXPECT_NEAR(dst[5], 0.5f, 1e-7f);
    EXPECT_NEAR(dst[6], -0.5f, 1e-7f);
    EXPECT_NEAR(dst[7], 0.25f, 1e-7f);
}

// ==============================================================================
// 2. PCM 24-Bit Packed Little-Endian Dequantization Tests
// ==============================================================================

TEST_CASE("converter_int24_packed_boundaries") {
    // 1. Zero: {0x00, 0x00, 0x00}
    const uint8_t zeroBytes[3] = {0x00, 0x00, 0x00};
    EXPECT_NEAR(dequantizeInt24Packed(zeroBytes), 0.0f, 1e-9f);

    // 2. Full-scale minimum: -8388608 -> {0x00, 0x00, 0x80}
    const uint8_t minBytes[3] = {0x00, 0x00, 0x80};
    EXPECT_NEAR(dequantizeInt24Packed(minBytes), -1.0f, 1e-9f);

    // 3. Full-scale maximum: +8388607 -> {0xFF, 0xFF, 0x7F}
    const uint8_t maxBytes[3] = {0xFF, 0xFF, 0x7F};
    constexpr float kMaxInt24Float = 8388607.0f / 8388608.0f;
    EXPECT_NEAR(dequantizeInt24Packed(maxBytes), kMaxInt24Float, 1e-9f);
    ASSERT_LT(dequantizeInt24Packed(maxBytes), 1.0f);

    // 4. Smallest positive: +1 -> {0x01, 0x00, 0x00}
    const uint8_t pos1Bytes[3] = {0x01, 0x00, 0x00};
    EXPECT_NEAR(dequantizeInt24Packed(pos1Bytes), kScaleInt24, 1e-9f);

    // 5. Negative one: -1 -> {0xFF, 0xFF, 0xFF}
    const uint8_t neg1Bytes[3] = {0xFF, 0xFF, 0xFF};
    EXPECT_NEAR(dequantizeInt24Packed(neg1Bytes), -kScaleInt24, 1e-9f);

    // 6. Negative two: -2 -> {0xFE, 0xFF, 0xFF}
    const uint8_t neg2Bytes[3] = {0xFE, 0xFF, 0xFF};
    EXPECT_NEAR(dequantizeInt24Packed(neg2Bytes), -2.0f * kScaleInt24, 1e-9f);

    // 7. Half-scale positive: +4194304 -> {0x00, 0x00, 0x40}
    const uint8_t halfPos[3] = {0x00, 0x00, 0x40};
    EXPECT_NEAR(dequantizeInt24Packed(halfPos), 0.5f, 1e-7f);

    // 8. Half-scale negative: -4194304 -> {0x00, 0x00, 0xC0}
    const uint8_t halfNeg[3] = {0x00, 0x00, 0xC0};
    EXPECT_NEAR(dequantizeInt24Packed(halfNeg), -0.5f, 1e-7f);

    // 9. Sign flip boundary: -8388607 -> {0x01, 0x00, 0x80}
    const uint8_t minPlus1Bytes[3] = {0x01, 0x00, 0x80};
    EXPECT_NEAR(dequantizeInt24Packed(minPlus1Bytes), -8388607.0f / 8388608.0f, 1e-7f);
}

TEST_CASE("converter_int24_packed_buffer_conversion") {
    // Contiguous sequence of 5 packed 24-bit samples:
    // Sample 0: 0
    // Sample 1: +8388607 (Max)
    // Sample 2: -8388608 (Min)
    // Sample 3: +1
    // Sample 4: -1
    const std::vector<uint8_t> raw = {
        0x00, 0x00, 0x00, // S0: 0
        0xFF, 0xFF, 0x7F, // S1: Max
        0x00, 0x00, 0x80, // S2: Min
        0x01, 0x00, 0x00, // S3: +1
        0xFF, 0xFF, 0xFF  // S4: -1
    };
    std::vector<float> dst(5, 0.0f);

    convertInt24ToFloat(raw.data(), dst.data(), 5);

    EXPECT_NEAR(dst[0], 0.0f, 1e-9f);
    EXPECT_NEAR(dst[1], 8388607.0f / 8388608.0f, 1e-9f);
    EXPECT_NEAR(dst[2], -1.0f, 1e-9f);
    EXPECT_NEAR(dst[3], kScaleInt24, 1e-9f);
    EXPECT_NEAR(dst[4], -kScaleInt24, 1e-9f);
}

// ==============================================================================
// 3. PCM 32-Bit Normalization & Boundary Tests
// ==============================================================================

TEST_CASE("converter_int32_exact_boundaries") {
    // 1. Zero
    EXPECT_NEAR(dequantizeInt32(0), 0.0f, 1e-9f);

    // 2. Full-scale minimum (INT32_MIN) -> exactly -1.0f
    EXPECT_NEAR(dequantizeInt32(std::numeric_limits<int32_t>::min()), -1.0f, 1e-9f);

    // 3. Full-scale maximum (INT32_MAX)
    constexpr float kMaxInt32Float = 2147483647.0f / 2147483648.0f;
    EXPECT_NEAR(dequantizeInt32(std::numeric_limits<int32_t>::max()), kMaxInt32Float, 1e-7f);
    ASSERT_LE(dequantizeInt32(std::numeric_limits<int32_t>::max()), 1.0f);

    // 4. Positive and negative unit steps
    EXPECT_NEAR(dequantizeInt32(1), kScaleInt32, 1e-11f);
    EXPECT_NEAR(dequantizeInt32(-1), -kScaleInt32, 1e-11f);

    // 5. Half scale points
    EXPECT_NEAR(dequantizeInt32(1073741824), 0.5f, 1e-7f);
    EXPECT_NEAR(dequantizeInt32(-1073741824), -0.5f, 1e-7f);
}

TEST_CASE("converter_int32_buffer_conversion") {
    const std::vector<int32_t> raw = {
        0, 1, -1,
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
        1073741824, -1073741824
    };
    std::vector<float> dst(raw.size(), 0.0f);

    convertInt32ToFloat(
        reinterpret_cast<const uint8_t*>(raw.data()),
        dst.data(),
        raw.size()
    );

    EXPECT_NEAR(dst[0], 0.0f, 1e-9f);
    EXPECT_NEAR(dst[1], kScaleInt32, 1e-9f);
    EXPECT_NEAR(dst[2], -kScaleInt32, 1e-9f);
    EXPECT_NEAR(dst[3], 2147483647.0f / 2147483648.0f, 1e-7f);
    EXPECT_NEAR(dst[4], -1.0f, 1e-9f);
    EXPECT_NEAR(dst[5], 0.5f, 1e-7f);
    EXPECT_NEAR(dst[6], -0.5f, 1e-7f);
}

// ==============================================================================
// 4. Float32 IEEE 754 Sanitization & Clamping Tests
// ==============================================================================

TEST_CASE("converter_float32_sanitization_and_clamping_suite") {
    // 1. Normal values in legal range
    EXPECT_NEAR(sanitizeFloat32(0.0f), 0.0f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(0.75f), 0.75f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(-0.75f), -0.75f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(1.0f), 1.0f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(-1.0f), -1.0f, 1e-7f);

    // 2. Positive and negative clipping
    EXPECT_NEAR(sanitizeFloat32(1.0001f), 1.0f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(50.0f), 1.0f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(-1.0001f), -1.0f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(-100.0f), -1.0f, 1e-7f);

    // 3. Quiet NaN -> sanitized to 0.0f
    const float qNaN = std::numeric_limits<float>::quiet_NaN();
    EXPECT_NEAR(sanitizeFloat32(qNaN), 0.0f, 1e-7f);

    // 4. Signaling NaN -> sanitized to 0.0f
    const float sNaN = std::numeric_limits<float>::signaling_NaN();
    EXPECT_NEAR(sanitizeFloat32(sNaN), 0.0f, 1e-7f);

    // 5. Positive and Negative Infinity -> sanitized to 0.0f
    const float posInf = std::numeric_limits<float>::infinity();
    const float negInf = -std::numeric_limits<float>::infinity();
    EXPECT_NEAR(sanitizeFloat32(posInf), 0.0f, 1e-7f);
    EXPECT_NEAR(sanitizeFloat32(negInf), 0.0f, 1e-7f);
}

TEST_CASE("converter_float32_buffer_conversion") {
    const std::vector<float> raw = {
        0.25f, -0.25f, 1.5f, -2.5f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        0.0f
    };
    std::vector<float> dst(raw.size(), 99.0f);

    convertFloat32Sanitized(
        reinterpret_cast<const uint8_t*>(raw.data()),
        dst.data(),
        raw.size()
    );

    EXPECT_NEAR(dst[0], 0.25f, 1e-7f);
    EXPECT_NEAR(dst[1], -0.25f, 1e-7f);
    EXPECT_NEAR(dst[2], 1.0f, 1e-7f);  // Clamped
    EXPECT_NEAR(dst[3], -1.0f, 1e-7f); // Clamped
    EXPECT_NEAR(dst[4], 0.0f, 1e-7f);  // NaN sanitized
    EXPECT_NEAR(dst[5], 0.0f, 1e-7f);  // +Inf sanitized
    EXPECT_NEAR(dst[6], 0.0f, 1e-7f);  // -Inf sanitized
    EXPECT_NEAR(dst[7], 0.0f, 1e-7f);
}

// ==============================================================================
// 5. Channel Interleaving & Deinterleaving Roundtrip Tests
// ==============================================================================

TEST_CASE("channel_stereo_interleaving_roundtrip") {
    constexpr size_t kFrames = 8;
    std::vector<float> origLeft = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    std::vector<float> origRight = {-0.1f, -0.2f, -0.3f, -0.4f, -0.5f, -0.6f, -0.7f, -0.8f};

    std::vector<float> interleaved(kFrames * 2, 0.0f);
    interleaveStereo(origLeft.data(), origRight.data(), interleaved.data(), kFrames);

    // Verify interleaved structure: [L0, R0, L1, R1, ...]
    for (size_t f = 0; f < kFrames; ++f) {
        EXPECT_NEAR(interleaved[f * 2 + 0], origLeft[f], 1e-7f);
        EXPECT_NEAR(interleaved[f * 2 + 1], origRight[f], 1e-7f);
    }

    std::vector<float> outLeft(kFrames, 0.0f);
    std::vector<float> outRight(kFrames, 0.0f);
    deinterleaveStereo(interleaved.data(), outLeft.data(), outRight.data(), kFrames);

    for (size_t f = 0; f < kFrames; ++f) {
        EXPECT_NEAR(outLeft[f], origLeft[f], 1e-7f);
        EXPECT_NEAR(outRight[f], origRight[f], 1e-7f);
    }
}

TEST_CASE("channel_multichannel_planar_roundtrip") {
    constexpr size_t kFrames = 4;
    constexpr size_t kChannels = 6; // 5.1 Surround sound
    constexpr size_t kTotal = kFrames * kChannels;

    std::vector<float> interleaved(kTotal);
    for (size_t f = 0; f < kFrames; ++f) {
        for (size_t ch = 0; ch < kChannels; ++ch) {
            interleaved[f * kChannels + ch] = static_cast<float>(ch * 10 + f) * 0.01f;
        }
    }

    std::vector<float> planar(kTotal, 0.0f);
    deinterleave(interleaved.data(), planar.data(), kFrames, kChannels);

    // Verify planar ordering: Channel 0 for all frames, then Channel 1, etc.
    for (size_t ch = 0; ch < kChannels; ++ch) {
        for (size_t f = 0; f < kFrames; ++f) {
            const float expected = static_cast<float>(ch * 10 + f) * 0.01f;
            EXPECT_NEAR(planar[ch * kFrames + f], expected, 1e-7f);
        }
    }

    std::vector<float> reconstructed(kTotal, 0.0f);
    interleave(planar.data(), reconstructed.data(), kFrames, kChannels);

    for (size_t i = 0; i < kTotal; ++i) {
        EXPECT_NEAR(reconstructed[i], interleaved[i], 1e-7f);
    }
}

TEST_CASE("channel_telemetry_peak_levels") {
    const std::vector<float> interleaved = {
        0.1f, -0.2f,
        0.6f, -0.4f,
        -0.85f, 0.3f,
        0.5f, -0.92f
    };

    float peakL = 0.0f;
    float peakR = 0.0f;
    calculatePeakLevelsStereo(interleaved.data(), 4, peakL, peakR);

    EXPECT_NEAR(peakL, 0.85f, 1e-7f);
    EXPECT_NEAR(peakR, 0.92f, 1e-7f);
}

// ==============================================================================
// 6. Unified Payload Dispatcher Tests
// ==============================================================================

TEST_CASE("converter_unified_dispatcher_int16") {
    const int16_t sample = 16384;
    float out = 0.0f;
    const bool ok = convertVbanPayloadToFloat(
        static_cast<uint8_t>(VbanAudioFormat::Int16),
        reinterpret_cast<const uint8_t*>(&sample),
        &out,
        1
    );
    EXPECT_TRUE(ok);
    EXPECT_NEAR(out, 0.5f, 1e-7f);
}

TEST_CASE("converter_unified_dispatcher_int24") {
    const uint8_t sample[3] = {0x00, 0x00, 0x80}; // Min (-8388608)
    float out = 0.0f;
    const bool ok = convertVbanPayloadToFloat(
        static_cast<uint8_t>(VbanAudioFormat::Int24),
        sample,
        &out,
        1
    );
    EXPECT_TRUE(ok);
    EXPECT_NEAR(out, -1.0f, 1e-7f);
}

TEST_CASE("converter_unified_dispatcher_int32") {
    const int32_t sample = 1073741824;
    float out = 0.0f;
    const bool ok = convertVbanPayloadToFloat(
        static_cast<uint8_t>(VbanAudioFormat::Int32),
        reinterpret_cast<const uint8_t*>(&sample),
        &out,
        1
    );
    EXPECT_TRUE(ok);
    EXPECT_NEAR(out, 0.5f, 1e-7f);
}

TEST_CASE("converter_unified_dispatcher_float32") {
    const float sample = 1.8f; // Overshoot
    float out = 0.0f;
    const bool ok = convertVbanPayloadToFloat(
        static_cast<uint8_t>(VbanAudioFormat::Float32),
        reinterpret_cast<const uint8_t*>(&sample),
        &out,
        1
    );
    EXPECT_TRUE(ok);
    EXPECT_NEAR(out, 1.0f, 1e-7f);
}

TEST_CASE("converter_unified_dispatcher_invalid_format") {
    const uint8_t dummy[4] = {0};
    float out = 0.0f;
    const bool ok = convertVbanPayloadToFloat(
        0x07, // Reserved / unsupported format code
        dummy,
        &out,
        1
    );
    EXPECT_FALSE(ok);
}
