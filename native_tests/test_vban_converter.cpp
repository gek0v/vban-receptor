#include "test_framework.hpp"
#include "vban_converter.hpp"

#include <vector>
#include <cmath>
#include <limits>
#include <cstring>
#include <bit>

namespace {
inline uint32_t floatToBits(float f) noexcept {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(float));
    return u;
}
} // anonymous namespace

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
// 5. Channel Telemetry Tests
// ==============================================================================

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

// ==============================================================================
// 7. SIMD Vectorized vs. Scalar Reference Oracle Bit-Exactness & Microbenchmarks
// ==============================================================================

TEST_CASE("converter_int16_bit_exactness_vector_vs_scalar_oracle") {
    const size_t testSizes[] = {
        1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 63, 64, 127, 128, 255, 256, 1024
    };

    for (size_t size : testSizes) {
        std::vector<int16_t> raw(size);
        for (size_t i = 0; i < size; ++i) {
            // Mix of edge cases and arithmetic progression
            if (i == 0) raw[i] = 0;
            else if (i == 1) raw[i] = 32767;
            else if (i == 2) raw[i] = -32768;
            else if (i == 3) raw[i] = 1;
            else if (i == 4) raw[i] = -1;
            else raw[i] = static_cast<int16_t>((i * 1337 + 42) % 65536 - 32768);
        }

        std::vector<float> dstVector(size, 0.0f);
        std::vector<float> dstScalar(size, 0.0f);

        convertInt16ToFloat(
            reinterpret_cast<const uint8_t*>(raw.data()),
            dstVector.data(),
            size
        );
        convertInt16ToFloatScalar(
            reinterpret_cast<const uint8_t*>(raw.data()),
            dstScalar.data(),
            size
        );

        for (size_t i = 0; i < size; ++i) {
            // Verify bit-exactness between dispatch path and scalar oracle
            EXPECT_EQ(floatToBits(dstVector[i]), floatToBits(dstScalar[i]));
            // Verify mathematical correctness against single-sample primitive
            EXPECT_EQ(floatToBits(dstVector[i]), floatToBits(dequantizeInt16(raw[i])));
        }
    }

    // Unaligned source pointer test (odd byte alignment)
    constexpr size_t kUnalignedSamples = 33;
    std::vector<uint8_t> unalignedBuf(kUnalignedSamples * 2 + 1, 0);
    uint8_t* unalignedSrc = unalignedBuf.data() + 1;
    for (size_t i = 0; i < kUnalignedSamples; ++i) {
        int16_t val = static_cast<int16_t>(i * 500 - 8000);
        std::memcpy(unalignedSrc + i * 2, &val, sizeof(int16_t));
    }
    std::vector<float> dstVec(kUnalignedSamples, 0.0f);
    std::vector<float> dstSca(kUnalignedSamples, 0.0f);
    convertInt16ToFloat(unalignedSrc, dstVec.data(), kUnalignedSamples);
    convertInt16ToFloatScalar(unalignedSrc, dstSca.data(), kUnalignedSamples);
    for (size_t i = 0; i < kUnalignedSamples; ++i) {
        EXPECT_EQ(floatToBits(dstVec[i]), floatToBits(dstSca[i]));
    }
}

TEST_CASE("converter_int24_bit_exactness_vector_vs_scalar_oracle") {
    const size_t testSizes[] = {
        1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 63, 64, 127, 128, 255, 256, 1024
    };

    for (size_t size : testSizes) {
        std::vector<uint8_t> raw(size * 3);
        for (size_t i = 0; i < size; ++i) {
            uint32_t val24 = 0;
            if (i == 0) val24 = 0x000000;              // 0
            else if (i == 1) val24 = 0x7FFFFF;         // +8388607 (Max)
            else if (i == 2) val24 = 0x800000;         // -8388608 (Min)
            else if (i == 3) val24 = 0x000001;         // +1
            else if (i == 4) val24 = 0xFFFFFF;         // -1
            else if (i == 5) val24 = 0x400000;         // +0.5
            else if (i == 6) val24 = 0xC00000;         // -0.5
            else val24 = static_cast<uint32_t>(i * 0x1234567 + 0xABCDEF) & 0x00FFFFFF;

            raw[i * 3 + 0] = static_cast<uint8_t>(val24 & 0xFF);
            raw[i * 3 + 1] = static_cast<uint8_t>((val24 >> 8) & 0xFF);
            raw[i * 3 + 2] = static_cast<uint8_t>((val24 >> 16) & 0xFF);
        }

        std::vector<float> dstVector(size, 0.0f);
        std::vector<float> dstScalar(size, 0.0f);

        convertInt24ToFloat(raw.data(), dstVector.data(), size);
        convertInt24ToFloatScalar(raw.data(), dstScalar.data(), size);

        for (size_t i = 0; i < size; ++i) {
            EXPECT_EQ(floatToBits(dstVector[i]), floatToBits(dstScalar[i]));
            EXPECT_EQ(
                floatToBits(dstVector[i]),
                floatToBits(dequantizeInt24Packed(&raw[i * 3]))
            );
        }
    }
}

TEST_CASE("converter_simd_design_and_arithmetic_oracle_proof") {
    // Mathematical equivalence proof of the NEON vector unpack arithmetic:
    // NEON shifts (B2 << 24) | (B1 << 16) | (B0 << 8) as uint32_t,
    // then performs SSHR (arithmetic right shift) by 8.
    // Verify that for all byte permutations, this is identical to:
    // static_cast<int32_t>(raw24 << 8) >> 8
    const uint32_t testValues[] = {
        0x000000, 0x000001, 0x000002, 0x7FFFFE, 0x7FFFFF, // Positive boundaries
        0x800000, 0x800001, 0xFFFFFE, 0xFFFFFF,           // Negative boundaries
        0x400000, 0xC00000, 0x123456, 0x876543, 0x555555, 0xAAAAAA
    };

    for (uint32_t val24 : testValues) {
        const uint8_t b0 = static_cast<uint8_t>(val24 & 0xFF);
        const uint8_t b1 = static_cast<uint8_t>((val24 >> 8) & 0xFF);
        const uint8_t b2 = static_cast<uint8_t>((val24 >> 16) & 0xFF);

        // Vector pipeline formula:
        const uint32_t neonShifted = (static_cast<uint32_t>(b2) << 24) |
                                     (static_cast<uint32_t>(b1) << 16) |
                                     (static_cast<uint32_t>(b0) << 8);
        const int32_t neonS32 = static_cast<int32_t>(neonShifted) >> 8;

        // Scalar reference formula:
        const uint32_t raw24 = static_cast<uint32_t>(b0) |
                              (static_cast<uint32_t>(b1) << 8) |
                              (static_cast<uint32_t>(b2) << 16);
        const int32_t scalarS32 = static_cast<int32_t>(raw24 << 8) >> 8;

        ASSERT_EQ(neonS32, scalarS32);
        EXPECT_EQ(
            floatToBits(static_cast<float>(neonS32) * kScaleInt24),
            floatToBits(static_cast<float>(scalarS32) * kScaleInt24)
        );
    }
}

TEST_CASE("converter_peak_levels_stereo_bit_exactness_vector_vs_scalar_oracle") {
    const size_t frameCounts[] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 63, 64, 127, 128, 255, 256, 512
    };

    for (size_t frames : frameCounts) {
        std::vector<float> stereo(frames * 2);
        for (size_t f = 0; f < frames; ++f) {
            float l = std::sin(static_cast<float>(f) * 0.1f) * 0.9f;
            float r = -std::cos(static_cast<float>(f) * 0.15f) * 0.85f;
            if (f == 10 && frames > 10) l = 1.5f; // Clamped peak test
            if (f == 20 && frames > 20) r = -1.8f;
            stereo[f * 2 + 0] = l;
            stereo[f * 2 + 1] = r;
        }

        float peakL_vec = 99.0f, peakR_vec = 99.0f;
        float peakL_sca = 88.0f, peakR_sca = 88.0f;

        calculatePeakLevelsStereo(stereo.data(), frames, peakL_vec, peakR_vec);
        calculatePeakLevelsStereoScalar(stereo.data(), frames, peakL_sca, peakR_sca);

        EXPECT_EQ(floatToBits(peakL_vec), floatToBits(peakL_sca));
        EXPECT_EQ(floatToBits(peakR_vec), floatToBits(peakR_sca));
    }
}

TEST_CASE("converter_throughput_microbenchmark_1m_samples") {
    constexpr size_t kBenchmarkSamples = 1000000; // 1,000,000 samples

    // 1. Benchmark Int16 to Float32 conversion
    std::vector<int16_t> src16(kBenchmarkSamples);
    std::vector<float> dst16(kBenchmarkSamples, 0.0f);
    for (size_t i = 0; i < kBenchmarkSamples; ++i) {
        src16[i] = static_cast<int16_t>(i % 32767);
    }

    constexpr size_t kIterations = 5;

    auto start16 = std::chrono::high_resolution_clock::now();
    for (size_t it = 0; it < kIterations; ++it) {
        convertInt16ToFloat(
            reinterpret_cast<const uint8_t*>(src16.data()),
            dst16.data(),
            kBenchmarkSamples
        );
    }
    auto end16 = std::chrono::high_resolution_clock::now();
    double totalUs16 = std::chrono::duration<double, std::micro>(end16 - start16).count();
    double ms16 = (totalUs16 / static_cast<double>(kIterations)) * 1e-3;
    if (ms16 < 0.001) ms16 = 0.001;
    double mSamplesPerSec16 = (static_cast<double>(kBenchmarkSamples) / (ms16 * 1e-3)) / 1e6;

    // Verify output integrity
    ASSERT_GT(dst16[100], -1.01f);
    ASSERT_LT(dst16[100], 1.01f);
    ASSERT_LT(ms16, 200.0); // Strict performance invariant: 1M samples under 200 ms

    // 2. Benchmark Int24 Packed to Float32 conversion
    std::vector<uint8_t> src24(kBenchmarkSamples * 3);
    std::vector<float> dst24(kBenchmarkSamples, 0.0f);
    for (size_t i = 0; i < kBenchmarkSamples; ++i) {
        uint32_t sample = static_cast<uint32_t>(i * 37) & 0x00FFFFFF;
        src24[i * 3 + 0] = static_cast<uint8_t>(sample & 0xFF);
        src24[i * 3 + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
        src24[i * 3 + 2] = static_cast<uint8_t>((sample >> 16) & 0xFF);
    }

    auto start24 = std::chrono::high_resolution_clock::now();
    for (size_t it = 0; it < kIterations; ++it) {
        convertInt24ToFloat(
            src24.data(),
            dst24.data(),
            kBenchmarkSamples
        );
    }
    auto end24 = std::chrono::high_resolution_clock::now();
    double totalUs24 = std::chrono::duration<double, std::micro>(end24 - start24).count();
    double ms24 = (totalUs24 / static_cast<double>(kIterations)) * 1e-3;
    if (ms24 < 0.001) ms24 = 0.001;
    double mSamplesPerSec24 = (static_cast<double>(kBenchmarkSamples) / (ms24 * 1e-3)) / 1e6;

    ASSERT_GT(dst24[100], -1.01f);
    ASSERT_LT(dst24[100], 1.01f);
    ASSERT_LT(ms24, 200.0);

    // 3. Benchmark Peak Level Detection on 500k stereo frames (1M floats)
    std::vector<float> stereoFloats(kBenchmarkSamples);
    for (size_t i = 0; i < kBenchmarkSamples; ++i) {
        stereoFloats[i] = dst24[i];
    }
    float peakL = 0.0f, peakR = 0.0f;
    auto startPeak = std::chrono::high_resolution_clock::now();
    for (size_t it = 0; it < kIterations; ++it) {
        calculatePeakLevelsStereo(
            stereoFloats.data(),
            kBenchmarkSamples / 2,
            peakL,
            peakR
        );
    }
    auto endPeak = std::chrono::high_resolution_clock::now();
    double totalUsPeak = std::chrono::duration<double, std::micro>(endPeak - startPeak).count();
    double msPeak = (totalUsPeak / static_cast<double>(kIterations)) * 1e-3;
    if (msPeak < 0.001) msPeak = 0.001;
    double mFloatsPerSecPeak = (static_cast<double>(kBenchmarkSamples) / (msPeak * 1e-3)) / 1e6;

    ASSERT_GE(peakL, 0.0f);
    ASSERT_LE(peakL, 1.0f);
    ASSERT_GE(peakR, 0.0f);
    ASSERT_LE(peakR, 1.0f);
    ASSERT_LT(msPeak, 200.0);

    std::cout << "\n    [BENCHMARK] Int16 -> Float: " << ms16 << " ms ("
              << mSamplesPerSec16 << " MSamples/s)\n"
              << "    [BENCHMARK] Int24 -> Float: " << ms24 << " ms ("
              << mSamplesPerSec24 << " MSamples/s)\n"
              << "    [BENCHMARK] Peak Detection: " << msPeak << " ms ("
              << mFloatsPerSecPeak << " MFloats/s)\n";
}
