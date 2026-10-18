#include "test_framework.hpp"
#include "vban_protocol.hpp"
#include "vban_parser.hpp"
#include "vban_converter.hpp"
#include "spsc_ring_buffer.hpp"
#include "drift_compensator.hpp"
#include "vban_receiver.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
using challenger_sock_t = SOCKET;
constexpr challenger_sock_t CHALLENGER_INVALID_SOCK = INVALID_SOCKET;
inline int challenger_close_sock(challenger_sock_t s) { return closesocket(s); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using challenger_sock_t = int;
constexpr challenger_sock_t CHALLENGER_INVALID_SOCK = -1;
inline int challenger_close_sock(challenger_sock_t s) { return close(s); }
#endif

#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <limits>
#include <cstring>
#include <cmath>
#include <span>

using namespace vban;

// ==============================================================================
// 1. SPSC Ring Buffer High-Churn Concurrency & Variable Chunk Stress
// ==============================================================================

TEST_CASE("challenger_spsc_high_churn_variable_chunk_concurrency") {
    constexpr size_t kTotalSamples = 2000000; // 2 Million samples
    constexpr size_t kRingCapacity = 4096;
    SpscRingBuffer<uint32_t, kRingCapacity> buffer;

    // Fast PRNG for deterministic variable chunk sizing
    std::thread producer([&]() {
        uint32_t xorshift_state = 0x12345678;
        auto fast_rand_range = [&](uint32_t minVal, uint32_t maxVal) -> size_t {
            xorshift_state ^= xorshift_state << 13;
            xorshift_state ^= xorshift_state >> 17;
            xorshift_state ^= xorshift_state << 5;
            return minVal + (xorshift_state % (maxVal - minVal + 1));
        };

        uint32_t current = 0;
        std::vector<uint32_t> tempChunk(512);

        while (current < kTotalSamples) {
            size_t chunkSize = fast_rand_range(1, 512);
            size_t toProduce = std::min<size_t>(chunkSize, kTotalSamples - current);

            for (size_t i = 0; i < toProduce; ++i) {
                tempChunk[i] = current + static_cast<uint32_t>(i);
            }

            size_t written = buffer.write(tempChunk.data(), toProduce);
            current += static_cast<uint32_t>(written);

            if (written == 0) {
                std::this_thread::yield();
            }
        }
    });

    std::vector<uint32_t> received;
    received.reserve(kTotalSamples);

    std::thread consumer([&]() {
        uint32_t xorshift_state = 0x87654321;
        auto fast_rand_range = [&](uint32_t minVal, uint32_t maxVal) -> size_t {
            xorshift_state ^= xorshift_state << 13;
            xorshift_state ^= xorshift_state >> 17;
            xorshift_state ^= xorshift_state << 5;
            return minVal + (xorshift_state % (maxVal - minVal + 1));
        };

        std::vector<uint32_t> tempChunk(512);

        while (received.size() < kTotalSamples) {
            size_t reqSize = fast_rand_range(1, 384);
            size_t count = buffer.read(tempChunk.data(), reqSize);

            for (size_t i = 0; i < count; ++i) {
                received.push_back(tempChunk[i]);
            }

            if (count == 0) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kTotalSamples);
    for (size_t i = 0; i < kTotalSamples; ++i) {
        if (received[i] != static_cast<uint32_t>(i)) {
            ASSERT_EQ(received[i], static_cast<uint32_t>(i));
        }
    }
}

// ==============================================================================
// 2. SPSC Extreme Contention Stress (Capacity = 16, 1 Million Elements)
// ==============================================================================

TEST_CASE("challenger_spsc_tight_capacity_high_contention") {
    constexpr size_t kTotalSamples = 1000000; // 1M samples
    constexpr size_t kTightCapacity = 16;     // Very small buffer (high full/empty collision rate)
    SpscRingBuffer<uint32_t, kTightCapacity> buffer;

    std::thread producer([&]() {
        uint32_t current = 0;
        uint32_t chunk[7];
        while (current < kTotalSamples) {
            size_t n = std::min<size_t>(5, kTotalSamples - current);
            for (size_t i = 0; i < n; ++i) {
                chunk[i] = current + static_cast<uint32_t>(i);
            }
            size_t written = buffer.write(chunk, n);
            current += static_cast<uint32_t>(written);
            if (written == 0) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        uint32_t expected = 0;
        uint32_t chunk[7];
        while (expected < kTotalSamples) {
            size_t readCount = buffer.read(chunk, 7);
            for (size_t i = 0; i < readCount; ++i) {
                if (chunk[i] != expected) {
                    ASSERT_EQ(chunk[i], expected);
                }
                expected++;
            }
            if (readCount == 0) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_TRUE(buffer.empty());
    ASSERT_EQ(buffer.availableRead(), 0U);
    ASSERT_EQ(buffer.availableWrite(), kTightCapacity);
}

// ==============================================================================
// 3. SPSC Full Wrap-Around Pattern Invariants
// ==============================================================================

TEST_CASE("challenger_spsc_full_buffer_exact_wrap_pattern") {
    constexpr size_t kCap = 64;
    SpscRingBuffer<uint64_t, kCap> buf;

    std::vector<uint64_t> in(kCap);
    std::vector<uint64_t> out(kCap);

    for (uint64_t iteration = 0; iteration < 500; ++iteration) {
        for (size_t i = 0; i < kCap; ++i) {
            in[i] = iteration * 1000 + i;
        }

        size_t written = buf.write(in.data(), kCap);
        ASSERT_EQ(written, kCap);
        ASSERT_TRUE(buf.full());
        ASSERT_FALSE(buf.empty());
        ASSERT_EQ(buf.availableWrite(), 0U);
        ASSERT_EQ(buf.availableRead(), kCap);

        // Attempting to write even 1 element to a full buffer must fail
        uint64_t overflowElem = 0xDEADBEEF;
        ASSERT_EQ(buf.write(&overflowElem, 1), 0U);

        size_t readCount = buf.read(out.data(), kCap);
        ASSERT_EQ(readCount, kCap);
        ASSERT_TRUE(buf.empty());
        ASSERT_FALSE(buf.full());
        ASSERT_EQ(buf.availableRead(), 0U);
        ASSERT_EQ(buf.availableWrite(), kCap);

        // Attempting to read even 1 element from an empty buffer must fail
        uint64_t underflowElem = 0;
        ASSERT_EQ(buf.read(&underflowElem, 1), 0U);

        for (size_t i = 0; i < kCap; ++i) {
            ASSERT_EQ(out[i], in[i]);
        }
    }
}

// ==============================================================================
// 4. Audio Converter: Exhaustive Int16 Oracle (All 65,536 Values)
// ==============================================================================

TEST_CASE("challenger_converter_int16_exhaustive_oracle") {
    constexpr size_t kTotalInt16 = 65536;
    std::vector<int16_t> allInt16(kTotalInt16);
    std::vector<float> converted(kTotalInt16, 0.0f);

    for (size_t i = 0; i < kTotalInt16; ++i) {
        allInt16[i] = static_cast<int16_t>(i - 32768);
    }

    // Convert via buffer routine
    convertInt16ToFloat(
        reinterpret_cast<const uint8_t*>(allInt16.data()),
        converted.data(),
        kTotalInt16
    );

    // Verify against mathematical oracle: sample / 32768.0f
    for (size_t i = 0; i < kTotalInt16; ++i) {
        int16_t s = allInt16[i];
        float oracle = static_cast<float>(s) * (1.0f / 32768.0f);
        ASSERT_NEAR(converted[i], oracle, 1e-7f);
        ASSERT_GE(converted[i], -1.0f);
        ASSERT_LE(converted[i], 32767.0f / 32768.0f);
    }

    // Test unaligned source buffer (offset by 1 byte)
    std::vector<uint8_t> unalignedBytes(kTotalInt16 * 2 + 1);
    std::memcpy(unalignedBytes.data() + 1, allInt16.data(), kTotalInt16 * 2);

    std::vector<float> unalignedConverted(kTotalInt16, 0.0f);
    convertInt16ToFloat(
        unalignedBytes.data() + 1,
        unalignedConverted.data(),
        kTotalInt16
    );

    for (size_t i = 0; i < kTotalInt16; ++i) {
        ASSERT_NEAR(unalignedConverted[i], converted[i], 1e-7f);
    }
}

// ==============================================================================
// 5. Audio Converter: 100,000 Random Int24 Packed Oracle Fuzz
// ==============================================================================

TEST_CASE("challenger_converter_int24_packed_random_oracle_fuzz") {
    constexpr size_t kSamples = 100000;
    std::vector<uint8_t> packedBytes(kSamples * 3);
    std::vector<float> expectedFloats(kSamples);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int32_t> dist(-8388608, 8388607);

    for (size_t i = 0; i < kSamples; ++i) {
        int32_t s24 = dist(rng);
        uint32_t u24 = static_cast<uint32_t>(s24) & 0x00FFFFFFU;
        packedBytes[i * 3 + 0] = static_cast<uint8_t>(u24 & 0xFF);
        packedBytes[i * 3 + 1] = static_cast<uint8_t>((u24 >> 8) & 0xFF);
        packedBytes[i * 3 + 2] = static_cast<uint8_t>((u24 >> 16) & 0xFF);

        expectedFloats[i] = static_cast<float>(s24) * (1.0f / 8388608.0f);
    }

    std::vector<float> converted(kSamples, 0.0f);
    convertInt24ToFloat(packedBytes.data(), converted.data(), kSamples);

    for (size_t i = 0; i < kSamples; ++i) {
        ASSERT_NEAR(converted[i], expectedFloats[i], 1e-7f);
        ASSERT_GE(converted[i], -1.0f);
        ASSERT_LE(converted[i], 8388607.0f / 8388608.0f);
    }
}

// ==============================================================================
// 6. Audio Converter: Int32 Boundaries & Unaligned Pointer Oracle
// ==============================================================================

TEST_CASE("challenger_converter_int32_boundary_and_unaligned_oracle") {
    const std::vector<int32_t> testValues = {
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min() + 1,
        -16777216,
        -1073741824,
        -65536,
        -1,
        0,
        1,
        65536,
        1073741824,
        16777216,
        std::numeric_limits<int32_t>::max() - 1,
        std::numeric_limits<int32_t>::max()
    };

    std::vector<float> alignedOut(testValues.size());
    convertInt32ToFloat(
        reinterpret_cast<const uint8_t*>(testValues.data()),
        alignedOut.data(),
        testValues.size()
    );

    for (size_t i = 0; i < testValues.size(); ++i) {
        float expected = static_cast<float>(testValues[i]) * (1.0f / 2147483648.0f);
        ASSERT_NEAR(alignedOut[i], expected, 1e-7f);
    }

    // Test unaligned paths (offsets 1, 2, 3 bytes)
    for (size_t unalignedOffset = 1; unalignedOffset <= 3; ++unalignedOffset) {
        std::vector<uint8_t> unalignedMem(testValues.size() * sizeof(int32_t) + unalignedOffset);
        std::memcpy(unalignedMem.data() + unalignedOffset, testValues.data(), testValues.size() * sizeof(int32_t));

        std::vector<float> unalignedOut(testValues.size(), 0.0f);
        convertInt32ToFloat(
            unalignedMem.data() + unalignedOffset,
            unalignedOut.data(),
            testValues.size()
        );

        for (size_t i = 0; i < testValues.size(); ++i) {
            ASSERT_NEAR(unalignedOut[i], alignedOut[i], 1e-7f);
        }
    }
}

// ==============================================================================
// 7. Audio Converter: Float32 Adversarial IEEE 754 Sanitization
// ==============================================================================

TEST_CASE("challenger_converter_float32_adversarial_sanitization") {
    const std::vector<float> adversarialInputs = {
        0.0f,
        -0.0f,
        1e-38f,       // Denormal / subnormal float
        -1e-38f,
        1e-45f,
        0.999999f,
        1.0f,
        -1.0f,
        1.000001f,    // Slight overshoot
        -1.000001f,   // Slight undershoot
        5.0f,
        -10.0f,
        1e30f,        // Massive finite float
        -1e30f,
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::signaling_NaN()
    };

    std::vector<float> sanitized(adversarialInputs.size(), 99.0f);
    convertFloat32Sanitized(
        reinterpret_cast<const uint8_t*>(adversarialInputs.data()),
        sanitized.data(),
        adversarialInputs.size()
    );

    // Verify all outputs are valid, finite, and clamped to [-1.0f, 1.0f]
    for (size_t i = 0; i < sanitized.size(); ++i) {
        ASSERT_TRUE(std::isfinite(sanitized[i]));
        ASSERT_GE(sanitized[i], -1.0f);
        ASSERT_LE(sanitized[i], 1.0f);
    }

    // Specific boundary checks
    ASSERT_NEAR(sanitized[0], 0.0f, 1e-7f);
    ASSERT_NEAR(sanitized[1], 0.0f, 1e-7f);
    ASSERT_NEAR(sanitized[6], 1.0f, 1e-7f);
    ASSERT_NEAR(sanitized[7], -1.0f, 1e-7f);
    ASSERT_NEAR(sanitized[8], 1.0f, 1e-7f);  // Clamped
    ASSERT_NEAR(sanitized[9], -1.0f, 1e-7f); // Clamped
    ASSERT_NEAR(sanitized[12], 1.0f, 1e-7f); // Clamped
    ASSERT_NEAR(sanitized[13], -1.0f, 1e-7f);// Clamped
    ASSERT_NEAR(sanitized[14], 0.0f, 1e-7f); // +Inf -> 0
    ASSERT_NEAR(sanitized[15], 0.0f, 1e-7f); // -Inf -> 0
    ASSERT_NEAR(sanitized[16], 0.0f, 1e-7f); // qNaN -> 0
    ASSERT_NEAR(sanitized[17], 0.0f, 1e-7f); // sNaN -> 0

    // Test unaligned paths
    for (size_t offset = 1; offset <= 3; ++offset) {
        std::vector<uint8_t> unaligned(adversarialInputs.size() * sizeof(float) + offset);
        std::memcpy(unaligned.data() + offset, adversarialInputs.data(), adversarialInputs.size() * sizeof(float));

        std::vector<float> unalignedSanitized(adversarialInputs.size(), 99.0f);
        convertFloat32Sanitized(unaligned.data() + offset, unalignedSanitized.data(), adversarialInputs.size());

        for (size_t i = 0; i < sanitized.size(); ++i) {
            ASSERT_NEAR(unalignedSanitized[i], sanitized[i], 1e-7f);
        }
    }
}

// ==============================================================================
// 8. Audio Converter: Multichannel Interleaving / Deinterleaving Invariants
// ==============================================================================

TEST_CASE("challenger_converter_multichannel_edge_cases") {
    // Edge case: zero frames or null pointers must be handled gracefully without crashing
    float dummyIn[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float dummyOut[4] = {0.0f};
    deinterleave(static_cast<const float*>(nullptr), dummyOut, 4, 2);
    deinterleave(dummyIn, static_cast<float*>(nullptr), 4, 2);
    deinterleave(dummyIn, dummyOut, 0, 2);
    deinterleave(dummyIn, dummyOut, 4, 0);

    interleave(static_cast<const float*>(nullptr), dummyOut, 4, 2);
    interleave(dummyIn, static_cast<float*>(nullptr), 4, 2);
    interleave(dummyIn, dummyOut, 0, 2);
    interleave(dummyIn, dummyOut, 4, 0);

    // 1-channel (Mono) pass-through test
    constexpr size_t kMonoFrames = 32;
    std::vector<float> monoIn(kMonoFrames);
    for (size_t i = 0; i < kMonoFrames; ++i) monoIn[i] = static_cast<float>(i) * 0.01f;
    std::vector<float> monoPlanar(kMonoFrames, 0.0f);
    deinterleave(monoIn.data(), monoPlanar.data(), kMonoFrames, 1);
    for (size_t i = 0; i < kMonoFrames; ++i) {
        ASSERT_NEAR(monoPlanar[i], monoIn[i], 1e-7f);
    }

    // 8-channel (7.1 Surround) roundtrip test
    constexpr size_t kFrames = 17; // Odd frame count
    constexpr size_t kChannels = 8;
    constexpr size_t kTotal = kFrames * kChannels;

    std::vector<float> multiIn(kTotal);
    for (size_t i = 0; i < kTotal; ++i) {
        multiIn[i] = static_cast<float>(i * 3 + 1) * 0.001f;
    }

    std::vector<float> multiPlanar(kTotal, 0.0f);
    deinterleave(multiIn.data(), multiPlanar.data(), kFrames, kChannels);

    std::vector<float> multiReconstructed(kTotal, 0.0f);
    interleave(multiPlanar.data(), multiReconstructed.data(), kFrames, kChannels);

    for (size_t i = 0; i < kTotal; ++i) {
        ASSERT_NEAR(multiReconstructed[i], multiIn[i], 1e-7f);
    }
}

// ==============================================================================
// 9. VBAN Protocol & Parser: Edge Cases, Malformed Headers & Fuzzing
// ==============================================================================

TEST_CASE("challenger_protocol_header_and_fuzzing") {
    // 1. Packets of every size from 0 to 27 bytes must fail with PacketTooSmall
    VbanParser parser("Stream1");
    ParsedPacket outPacket;
    std::vector<uint8_t> buffer(30, 0);

    for (size_t sz = 0; sz < VBAN_HEADER_SIZE; ++sz) {
        ASSERT_EQ(parser.parse(buffer.data(), sz, outPacket), ParseResult::PacketTooSmall);
    }

    // 2. Stream Name 16-character full buffer without null terminator
    char full16[16] = {'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P'};
    std::string_view extracted16 = extractStreamName(full16);
    ASSERT_EQ(extracted16.size(), 16U);
    ASSERT_EQ(extracted16, "ABCDEFGHIJKLMNOP");

    // 3. Stream Name with only whitespace
    char spaceOnly[16] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    ASSERT_EQ(extractStreamName(spaceOnly), "");

    // 4. Stream Name with leading and embedded spaces: "  Hi World  " -> trailing spaces stripped, leading preserved
    char spaces[16] = {' ', ' ', 'H', 'i', ' ', 'W', 'o', 'r', 'l', 'd', ' ', ' ', 0, 0, 0, 0};
    ASSERT_EQ(extractStreamName(spaces), "  Hi World");

    // 5. Payload length calculation overflow safety
    // nbs = 255 (256 frames), nbc = 255 (256 channels), sampleSize = 8 (Float64)
    // 256 * 256 * 8 = 524,288 bytes
    size_t maxPayload = calculatePayloadBytes(255, 255, 8);
    ASSERT_EQ(maxPayload, 524288U);
    ASSERT_EQ(calculateTotalPacketBytes(255, 255, 8), 524288U + 28U);
}

TEST_CASE("challenger_parser_state_resilience") {
    VbanParser parser("Stream1");
    ParsedPacket outPacket;

    // Helper generating a valid 48kHz, 256 frame, stereo, 16-bit packet
    auto makePacket = [](uint32_t nuFrame) -> std::vector<uint8_t> {
        size_t totalBytes = calculateTotalPacketBytes(255, 1, 2); // 1052 bytes
        std::vector<uint8_t> pkt(totalBytes, 0);
        auto* hdr = reinterpret_cast<VBanHeader*>(pkt.data());
        hdr->magic = VBAN_MAGIC;
        hdr->format_SR = static_cast<uint8_t>(VBanSubProtocol::Audio) | 3; // 48 kHz
        hdr->format_nbs = 255; // 256 frames
        hdr->format_nbc = 1;   // 2 channels
        hdr->format_bit = static_cast<uint8_t>(VBanCodec::PCM) | static_cast<uint8_t>(VBanDataType::Int16);
        std::memcpy(hdr->streamName, "Stream1", 7);
        hdr->nuFrame = nuFrame;
        return pkt;
    };

    // 1. Initial packet
    auto p0 = makePacket(100);
    ASSERT_EQ(parser.parse(p0.data(), p0.size(), outPacket), ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 0U);

    // 2. Inject gaps and verify lost packets counter
    // Jump from 100 to 110 (delta 10 -> 9 lost)
    auto p1 = makePacket(110);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), outPacket), ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 9U);

    // Jump from 110 to 125 (delta 15 -> 14 lost -> total 23 lost)
    auto p2 = makePacket(125);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), outPacket), ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 23U);

    // 3. Inject out-of-order packets (nuFrame 115 and duplicate 125)
    auto pOld = makePacket(115);
    ASSERT_EQ(parser.parse(pOld.data(), pOld.size(), outPacket), ParseResult::Success);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 1U);
    ASSERT_EQ(parser.getLostPackets(), 23U); // Loss count does not change

    auto pDup = makePacket(125);
    ASSERT_EQ(parser.parse(pDup.data(), pDup.size(), outPacket), ParseResult::Success);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 2U);
    ASSERT_EQ(parser.getLostPackets(), 23U);

    // 4. Wrap-around across uint32 max: 0xFFFFFFF0 -> 0x0000000A (delta = 26, 25 lost)
    parser.reset();
    auto pWrap1 = makePacket(0xFFFFFFF0U);
    ASSERT_EQ(parser.parse(pWrap1.data(), pWrap1.size(), outPacket), ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    auto pWrap2 = makePacket(0x0000000AU);
    ASSERT_EQ(parser.parse(pWrap2.data(), pWrap2.size(), outPacket), ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 25U);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 0U);
}

// ==============================================================================
// Milestone 3 Challenger Adversarial Stress Tests: Clock Drift & Micro-Resampler
// ==============================================================================

TEST_CASE("challenger_m3_catmull_rom_multirate_thd_matrix") {
    const std::array<double, 3> sampleRates = {48000.0, 192000.0, 768000.0};
    const std::array<double, 4> driftPpmList = {-100.0, -50.0, +50.0, +100.0};
    const double f = 1000.0;
    const size_t N = 8000;
    constexpr double kPi = 3.14159265358979323846;

    for (double fs : sampleRates) {
        for (double driftPpm : driftPpmList) {
            const double ratio = 1.0 + driftPpm * 1e-6;
            std::vector<float> discrete(N + 32);
            for (size_t i = 0; i < discrete.size(); ++i) {
                discrete[i] = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(i) / fs));
            }

            double energySignal = 0.0;
            double energyError = 0.0;

            for (size_t n = 8; n < N - 8; ++n) {
                double t_virtual = static_cast<double>(n) * ratio;
                int64_t k = static_cast<int64_t>(std::floor(t_virtual));
                float frac = static_cast<float>(t_virtual - static_cast<double>(k));

                float p0 = discrete[k - 1];
                float p1 = discrete[k];
                float p2 = discrete[k + 1];
                float p3 = discrete[k + 2];

                float resampled = vban::CatmullRomResampler::interpolateScalar(p0, p1, p2, p3, frac);
                double analytic = std::sin(2.0 * kPi * f * t_virtual / fs);

                double err = static_cast<double>(resampled) - analytic;
                energySignal += analytic * analytic;
                energyError += err * err;
            }

            double thd_db = 10.0 * std::log10(energyError / energySignal);
            ASSERT_LT(thd_db, -90.0);
            if (fs >= 192000.0) {
                ASSERT_LT(thd_db, -120.0);
            }
            if (fs >= 768000.0) {
                ASSERT_LT(thd_db, -140.0);
            }
        }
    }
}

TEST_CASE("challenger_m3_catmull_rom_stereo_vector_equivalence") {
    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> valDist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> fracDist(0.0f, 1.0f);

    for (int iter = 0; iter < 1000; ++iter) {
        float p0[vban::CatmullRomResampler::MAX_CHANNELS] = {valDist(rng), valDist(rng)};
        float p1[vban::CatmullRomResampler::MAX_CHANNELS] = {valDist(rng), valDist(rng)};
        float p2[vban::CatmullRomResampler::MAX_CHANNELS] = {valDist(rng), valDist(rng)};
        float p3[vban::CatmullRomResampler::MAX_CHANNELS] = {valDist(rng), valDist(rng)};
        float t = fracDist(rng);

        float outStereo[2];
        vban::CatmullRomResampler::interpolateStereo(p0, p1, p2, p3, t, outStereo);

        float outScalarL = vban::CatmullRomResampler::interpolateScalar(p0[0], p1[0], p2[0], p3[0], t);
        float outScalarR = vban::CatmullRomResampler::interpolateScalar(p0[1], p1[1], p2[1], p3[1], t);

        ASSERT_NEAR(outStereo[0], outScalarL, 1e-6f);
        ASSERT_NEAR(outStereo[1], outScalarR, 1e-6f);
    }
}

TEST_CASE("challenger_m3_catmull_rom_ring_buffer_streaming_thd") {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double fs = 48000.0;
    constexpr double f = 1000.0;
    constexpr double driftPpm = 100.0;
    constexpr double ratio = 1.0 + driftPpm * 1e-6;

    vban::SpscRingBuffer<float, 65536> ringBuffer;
    vban::CatmullRomResampler resampler;
    resampler.reset();

    constexpr size_t totalFrames = 10000;
    std::vector<float> sineInput(totalFrames * 2);
    for (size_t i = 0; i < totalFrames; ++i) {
        float s = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(i) / fs));
        sineInput[i * 2 + 0] = s;
        sineInput[i * 2 + 1] = s;
    }

    size_t inWritten = ringBuffer.write(sineInput.data(), 4096 * 2);
    ASSERT_EQ(inWritten, 4096U * 2U);
    size_t inPos = 4096;

    std::vector<float> renderedAudio;
    renderedAudio.reserve(totalFrames * 2);
    std::vector<float> chunk(128 * 2);

    while (renderedAudio.size() < 4000 * 2) {
        if (ringBuffer.availableWrite() >= 512 * 2 && inPos + 256 <= totalFrames) {
            ringBuffer.write(&sineInput[inPos * 2], 256 * 2);
            inPos += 256;
        }

        size_t rendered = resampler.process(ringBuffer, chunk.data(), 128, 2, ratio);
        ASSERT_EQ(rendered, 128U);
        for (size_t i = 0; i < 128 * 2; ++i) {
            renderedAudio.push_back(chunk[i]);
        }
    }

    double energySignal = 0.0;
    double energyError = 0.0;
    size_t framesToTest = renderedAudio.size() / 2;

    for (size_t n = 256; n < framesToTest - 64; ++n) {
        double t_virtual = (1.0 + static_cast<double>(n) * ratio) / fs;
        double analytic = std::sin(2.0 * kPi * f * t_virtual);

        double actualL = static_cast<double>(renderedAudio[n * 2 + 0]);
        double actualR = static_cast<double>(renderedAudio[n * 2 + 1]);

        ASSERT_NEAR(actualL, actualR, 1e-6);

        double err = actualL - analytic;
        energySignal += analytic * analytic;
        energyError += err * err;
    }

    double thd_db = 10.0 * std::log10(energyError / energySignal);
    ASSERT_LT(thd_db, -90.0);
}

TEST_CASE("challenger_m3_ema_filter_critically_damped_no_overshoot") {
    vban::EmaFilter2ndOrder filter(8.0);
    filter.reset();
    filter.seed(0.0);

    const double stepVal = 100000.0;
    const double dt = 0.01;
    double prevVal = 0.0;

    for (int step = 0; step < 10000; ++step) {
        double val = filter.update(stepVal, dt);
        ASSERT_GE(val, prevVal);
        ASSERT_LE(val, stepVal);
        ASSERT_GE(filter.getFirstStageValue(), val);
        prevVal = val;
    }

    // Near convergence after 100 seconds (12.5 tau: analytic remaining is 5.03)
    ASSERT_NEAR(filter.getValue(), stepVal, 6.0);
}

TEST_CASE("challenger_m3_ema_wifi_jitter_rejection_stress") {
    vban::EmaFilter2ndOrder filter(8.0);
    filter.reset();
    filter.seed(131072.0);

    std::mt19937 rng(9999);
    std::uniform_real_distribution<double> normalJitter(-960.0, +960.0);
    std::uniform_real_distribution<double> severeBurst(-2400.0, +2400.0);
    std::bernoulli_distribution burstProb(0.05);

    const size_t numPackets = 20000;
    const double dt = 0.005;

    double sumRaw = 0.0, sumSqRaw = 0.0;
    double sumFilt = 0.0, sumSqFilt = 0.0;

    for (size_t i = 0; i < numPackets; ++i) {
        double j = burstProb(rng) ? severeBurst(rng) : normalJitter(rng);
        double raw = 131072.0 + j;
        double filtered = filter.update(raw, dt);

        sumRaw += raw; sumSqRaw += raw * raw;
        sumFilt += filtered; sumSqFilt += filtered * filtered;
    }

    double varRaw = (sumSqRaw - (sumRaw * sumRaw) / numPackets) / numPackets;
    double varFilt = (sumSqFilt - (sumFilt * sumFilt) / numPackets) / numPackets;
    double reductionPercent = (1.0 - varFilt / varRaw) * 100.0;

    ASSERT_GT(reductionPercent, 98.0);
}

TEST_CASE("challenger_m3_pi_servo_anti_windup_instant_recovery") {
    vban::PiServoController pi(2.0, 0.05, 100.0);
    pi.reset();

    for (int i = 0; i < 5000; ++i) {
        pi.update(200.0, 0.05);
    }
    ASSERT_EQ(pi.getLastOutput(), 100.0);
    ASSERT_LE(pi.getIntegral(), 100.0);

    double outAfterRev = pi.update(-2.0, 0.05);
    ASSERT_LT(outAfterRev, 100.0);

    pi.reset();
    for (int i = 0; i < 5000; ++i) {
        pi.update(-200.0, 0.05);
    }
    ASSERT_EQ(pi.getLastOutput(), -100.0);
    ASSERT_GE(pi.getIntegral(), -100.0);

    double outAfterPos = pi.update(+2.0, 0.05);
    ASSERT_GT(outAfterPos, -100.0);
}

TEST_CASE("challenger_m3_slew_rate_limiter_invariance_and_gradient") {
    vban::SlewRateLimiter limiter(0.1, 100.0);
    limiter.seed(0.0);

    std::mt19937 rng(777);
    std::uniform_real_distribution<double> targetDist(-150.0, +150.0);
    std::uniform_real_distribution<double> dtDist(1e-5, 2.0);

    double currentVal = 0.0;

    for (int i = 0; i < 1000; ++i) {
        double target = targetDist(rng);
        double dt = dtDist(rng);

        double nextVal = limiter.update(target, dt);
        double maxAllowedDelta = 0.1 * dt + 1e-9;
        double actualDelta = std::abs(nextVal - currentVal);

        ASSERT_LE(actualDelta, maxAllowedDelta);
        ASSERT_LE(std::abs(nextVal), 100.0);
        currentVal = nextVal;
    }
}

TEST_CASE("challenger_m3_fade_engine_rapid_reversals_and_emergency") {
    vban::FadeEngine engine;
    engine.reset();
    ASSERT_TRUE(engine.isMuted());

    std::vector<float> audio(256 * 2, 1.0f);

    engine.startFadeIn();
    engine.process(audio.data(), 30, 2);
    ASSERT_TRUE(engine.isTransitioning());
    float gainAt30 = audio[29 * 2];
    ASSERT_GT(gainAt30, 0.0f);
    ASSERT_LT(gainAt30, 0.5f);

    engine.startFadeOut();
    engine.process(audio.data(), 15, 2);
    ASSERT_TRUE(engine.isTransitioning());

    engine.startFadeIn();
    engine.process(audio.data(), 128, 2);
    ASSERT_TRUE(engine.isActive());

    std::fill(audio.begin(), audio.end(), 1.0f);
    engine.emergencyFadeOut(audio.data(), 0, 256, 2);
    ASSERT_TRUE(engine.isMuted());
    for (float sample : audio) {
        ASSERT_EQ(sample, 0.0f);
    }

    std::fill(audio.begin(), audio.end(), 1.0f);
    engine.setActive();
    engine.emergencyFadeOut(audio.data(), 1, 256, 2);
    ASSERT_TRUE(engine.isMuted());
    for (size_t i = 1 * 2; i < 256 * 2; ++i) {
        ASSERT_EQ(audio[i], 0.0f);
    }
}

// ==============================================================================
// 19. M3 Remediation Stress: Arbitrary Audio Callback Block Sizes
// ==============================================================================
TEST_CASE("challenger_m3_starvation_recovery_arbitrary_callback_sizes") {
    const std::vector<size_t> blockSizes = {16, 32, 48, 64, 96, 128, 192, 256};
    const size_t kChannels = 2;

    for (size_t blockSize : blockSizes) {
        vban::ClockDriftConfig cfg;
        cfg.ringBufferCapacity = 4096;
        cfg.targetOccupancyPercent = 50.0;
        vban::ClockDriftCompensator comp(cfg);
        comp.reset();

        vban::SpscRingBuffer<float, 4096> ringBuffer;

        // Step 1: Active streaming
        std::vector<float> audioIn(1024 * kChannels, 0.8f);
        ringBuffer.write(audioIn.data(), audioIn.size());
        comp.triggerFadeIn();

        // Warm up until active
        std::vector<float> outBuf(blockSize * kChannels);
        while (comp.getFadeEngine().getState() != vban::FadeState::Active) {
            size_t r = comp.process(ringBuffer, outBuf.data(), blockSize, kChannels);
            ASSERT_EQ(r, blockSize);
        }
        ASSERT_TRUE(comp.getFadeEngine().isActive());

        // Step 2: Starvation - drain ring buffer and staging buffer until starvation occurs
        while (ringBuffer.availableRead() > 0) {
            float dummy[64];
            ringBuffer.read(dummy, std::min(ringBuffer.availableRead(), size_t{64}));
        }

        // Drain any remaining frames inside CatmullRomResampler's staging buffer until starvation
        std::vector<float> drainBuf(blockSize * kChannels);
        while (comp.process(ringBuffer, drainBuf.data(), blockSize, kChannels) == blockSize) {
            // consuming remaining staged frames
        }

        // Now both ring buffer and staging buffer are completely empty.
        // Resampler must be unprimed and FadeEngine must be muted.
        ASSERT_TRUE(comp.getFadeEngine().isMuted());
        ASSERT_FALSE(comp.getResampler().isPrimed());

        // A subsequent call while completely starved must produce 0 frames and exact silence
        std::vector<float> starveBuf(blockSize * kChannels, 1.0f);
        size_t starveRendered = comp.process(ringBuffer, starveBuf.data(), blockSize, kChannels);
        ASSERT_EQ(starveRendered, 0U);
        ASSERT_TRUE(comp.getFadeEngine().isMuted());
        ASSERT_FALSE(comp.getResampler().isPrimed());
        for (float s : starveBuf) {
            ASSERT_EQ(s, 0.0f);
        }

        // Step 3: Refill with distinct 0.5f audio
        std::vector<float> refillAudio(2048 * kChannels, 0.5f);
        ringBuffer.write(refillAudio.data(), refillAudio.size());

        // Step 4: Recover and verify smooth cosine progression across multiple callbacks
        size_t totalRenderedFrames = 0;
        std::vector<float> fullRecoveredAudio;
        while (totalRenderedFrames < 256) {
            std::vector<float> recChunk(blockSize * kChannels, 0.0f);
            size_t r = comp.process(ringBuffer, recChunk.data(), blockSize, kChannels);
            ASSERT_EQ(r, blockSize);
            totalRenderedFrames += r;
            fullRecoveredAudio.insert(fullRecoveredAudio.end(), recChunk.begin(), recChunk.end());
            if (totalRenderedFrames >= 128) {
                ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::Active);
            }
        }

        // First sample MUST be 0.0f (raised cosine window start)
        ASSERT_NEAR(fullRecoveredAudio[0], 0.0f, 1e-4f);
        ASSERT_NEAR(fullRecoveredAudio[1], 0.0f, 1e-4f);

        // Verify monotonic ramp during 128-sample window (L & R channels)
        for (size_t f = 1; f < 128; ++f) {
            float prevSample = fullRecoveredAudio[(f - 1) * kChannels];
            float currSample = fullRecoveredAudio[f * kChannels];
            ASSERT_GE(currSample + 1e-5f, prevSample);
        }

        // Midpoint (sample 64) should be ~ half amplitude (0.5 * 0.5 = 0.25)
        ASSERT_NEAR(fullRecoveredAudio[64 * kChannels], 0.25f, 0.05f);

        // After window (samples >= 128), audio should be steady 0.5f
        for (size_t f = 128; f < totalRenderedFrames; ++f) {
            ASSERT_NEAR(fullRecoveredAudio[f * kChannels + 0], 0.5f, 0.02f);
            ASSERT_NEAR(fullRecoveredAudio[f * kChannels + 1], 0.5f, 0.02f);
        }
    }
}

// ==============================================================================
// 20. M3 Remediation Stress: Starvation Occurring During FadingIn State & Re-Recovery
// ==============================================================================
TEST_CASE("challenger_m3_starvation_during_fade_in_re_recovery") {
    vban::ClockDriftConfig cfg;
    cfg.ringBufferCapacity = 1024;
    cfg.targetOccupancyPercent = 50.0;
    vban::ClockDriftCompensator comp(cfg);
    comp.reset();

    vban::SpscRingBuffer<float, 1024> ringBuffer;
    const size_t kChannels = 2;

    // Start in starved/muted state
    std::vector<float> emptyBuf(64 * kChannels, 1.0f);
    comp.process(ringBuffer, emptyBuf.data(), 64, kChannels);
    ASSERT_TRUE(comp.getFadeEngine().isMuted());

    // Write exactly enough for priming (4) + 64 frames = 68 frames
    std::vector<float> partialAudio(68 * kChannels, 0.7f);
    ringBuffer.write(partialAudio.data(), partialAudio.size());

    // Callback 1 (64 frames): healthy -> triggers startFadeIn(), renders 64 frames (halfway)
    std::vector<float> chunk1(64 * kChannels, 0.0f);
    size_t r1 = comp.process(ringBuffer, chunk1.data(), 64, kChannels);
    ASSERT_EQ(r1, 64U);
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::FadingIn);
    ASSERT_TRUE(comp.getFadeEngine().isTransitioning());
    ASSERT_NEAR(chunk1[0], 0.0f, 1e-4f);

    // Callback 2 (64 frames): Ring buffer is now starved (only ~4 frames remain)!
    std::vector<float> chunk2(64 * kChannels, 1.0f);
    size_t r2 = comp.process(ringBuffer, chunk2.data(), 64, kChannels);
    ASSERT_LT(r2, 64U);
    // Comp must enter emergency fade out, unprime, and mute
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    ASSERT_FALSE(comp.getResampler().isPrimed());
    // Remainder after r2 frames must be 0.0f
    for (size_t i = r2 * kChannels; i < 64 * kChannels; ++i) {
        ASSERT_EQ(chunk2[i], 0.0f);
    }

    // Callback 3: Secondary starvation while empty
    std::vector<float> chunk3(64 * kChannels, 1.0f);
    size_t r3 = comp.process(ringBuffer, chunk3.data(), 64, kChannels);
    ASSERT_EQ(r3, 0U);
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    for (float s : chunk3) {
        ASSERT_EQ(s, 0.0f);
    }

    // Now refill with abundant audio (512 frames of 0.4f)
    std::vector<float> fullAudio(512 * kChannels, 0.4f);
    ringBuffer.write(fullAudio.data(), fullAudio.size());

    // Callback 4: Should cleanly restart fade-in from 0.0f
    std::vector<float> chunk4(64 * kChannels, 0.0f);
    size_t r4 = comp.process(ringBuffer, chunk4.data(), 64, kChannels);
    ASSERT_EQ(r4, 64U);
    ASSERT_TRUE(comp.getResampler().isPrimed());
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::FadingIn);
    ASSERT_NEAR(chunk4[0], 0.0f, 1e-4f);

    // Callback 5: Completes 128-window -> becomes Active
    std::vector<float> chunk5(64 * kChannels, 0.0f);
    size_t r5 = comp.process(ringBuffer, chunk5.data(), 64, kChannels);
    ASSERT_EQ(r5, 64U);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    ASSERT_NEAR(chunk5[(64 - 1) * kChannels], 0.4f, 0.05f);
}

// ==============================================================================
// 21. M3 Remediation Stress: Stale History Isolation & Zero Leakage
// ==============================================================================
TEST_CASE("challenger_m3_starvation_resampler_history_zero_leakage") {
    vban::ClockDriftConfig cfg;
    cfg.ringBufferCapacity = 1024;
    cfg.targetOccupancyPercent = 50.0;
    vban::ClockDriftCompensator comp(cfg);
    comp.reset();

    vban::SpscRingBuffer<float, 1024> ringBuffer;
    const size_t kChannels = 2;

    // Fill with positive +0.95f audio
    std::vector<float> posAudio(256 * kChannels, 0.95f);
    ringBuffer.write(posAudio.data(), posAudio.size());
    comp.triggerFadeIn();

    std::vector<float> warm(128 * kChannels);
    comp.process(ringBuffer, warm.data(), 128, kChannels);
    ASSERT_TRUE(comp.getResampler().isPrimed());

    // Force starvation
    while (ringBuffer.availableRead() > 0) {
        float dummy[64];
        ringBuffer.read(dummy, std::min(ringBuffer.availableRead(), size_t{64}));
    }

    std::vector<float> starveBuf(128 * kChannels);
    comp.process(ringBuffer, starveBuf.data(), 128, kChannels);
    ASSERT_FALSE(comp.getResampler().isPrimed());

    // Now write negative audio: -0.6f
    std::vector<float> negAudio(256 * kChannels, -0.6f);
    ringBuffer.write(negAudio.data(), negAudio.size());

    // Render recovered audio
    std::vector<float> recBuf(128 * kChannels, 0.0f);
    size_t r = comp.process(ringBuffer, recBuf.data(), 128, kChannels);
    ASSERT_EQ(r, 128U);
    ASSERT_TRUE(comp.getResampler().isPrimed());

    // Crucial check: not a single sample should have positive leakage from old +0.95f!
    // Since input is -0.6f and gain is [0.0, 1.0], all samples must be <= 0.0f!
    for (size_t i = 0; i < 128 * kChannels; ++i) {
        ASSERT_LE(recBuf[i], 1e-5f);
    }
}

// ==============================================================================
// Milestone 4 Challenger Adversarial Stress Tests: Network Receiver & JNI Bridge
// ==============================================================================

namespace {

struct ChallengerWsaGuard {
    ChallengerWsaGuard() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~ChallengerWsaGuard() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

inline std::vector<uint8_t> makeChallengerPacket(
    uint32_t srIndex,
    uint8_t nbs,
    uint8_t nbc,
    vban::VBanDataType dt,
    const char* streamName,
    uint32_t nuFrame) {

    uint8_t sampleSize = vban::getSampleSize(dt);
    size_t payloadBytes = vban::calculatePayloadBytes(nbs, nbc, sampleSize);
    std::vector<uint8_t> pkt(vban::VBAN_HEADER_SIZE + payloadBytes, 0);

    auto* hdr = reinterpret_cast<vban::VBanHeader*>(pkt.data());
    hdr->magic = vban::VBAN_MAGIC;
    hdr->format_SR = (static_cast<uint8_t>(vban::VBanSubProtocol::Audio)) | (srIndex & 0x1F);
    hdr->format_nbs = nbs;
    hdr->format_nbc = nbc;
    hdr->format_bit = (static_cast<uint8_t>(vban::VBanCodec::PCM)) | (static_cast<uint8_t>(dt) & 0x07);

    std::memset(hdr->streamName, 0, 16);
    if (streamName) {
        size_t n = std::min(std::strlen(streamName), static_cast<size_t>(16));
        std::memcpy(hdr->streamName, streamName, n);
    }
    hdr->nuFrame = nuFrame;

    // Fill with predictable waveform
    size_t totalSamples = (nbs + 1) * (nbc + 1);
    uint8_t* payload = pkt.data() + vban::VBAN_HEADER_SIZE;
    if (dt == vban::VBanDataType::Int16) {
        auto* s = reinterpret_cast<int16_t*>(payload);
        for (size_t i = 0; i < totalSamples; ++i) {
            s[i] = static_cast<int16_t>(8192.0 * std::sin(2.0 * 3.1415926535 * i / 32.0));
        }
    } else if (dt == vban::VBanDataType::Float32) {
        auto* s = reinterpret_cast<float*>(payload);
        for (size_t i = 0; i < totalSamples; ++i) {
            s[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.1415926535 * i / 32.0));
        }
    } else if (dt == vban::VBanDataType::Int32) {
        auto* s = reinterpret_cast<int32_t*>(payload);
        for (size_t i = 0; i < totalSamples; ++i) {
            s[i] = static_cast<int32_t>(536870912.0 * std::sin(2.0 * 3.1415926535 * i / 32.0));
        }
    } else if (dt == vban::VBanDataType::Int24) {
        for (size_t i = 0; i < totalSamples; ++i) {
            int32_t val = static_cast<int32_t>(2097152.0 * std::sin(2.0 * 3.1415926535 * i / 32.0));
            uint32_t u = static_cast<uint32_t>(val) & 0x00FFFFFFU;
            payload[i * 3 + 0] = static_cast<uint8_t>(u & 0xFF);
            payload[i * 3 + 1] = static_cast<uint8_t>((u >> 8) & 0xFF);
            payload[i * 3 + 2] = static_cast<uint8_t>((u >> 16) & 0xFF);
        }
    }

    return pkt;
}

} // anonymous namespace

// ==============================================================================
// 22. Multicast IPv4 Address Classification Boundaries & Adversarial Fuzzing
// ==============================================================================
TEST_CASE("challenger_m4_multicast_address_boundaries_and_fuzz") {
    // 1. Exact mathematical boundaries of IPv4 Class D (224.0.0.0/4 -> 0xE0000000 to 0xEFFFFFFF)
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("224.0.0.0"));        // Lowest Class D address
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("224.0.0.1"));        // All Systems multicast
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("224.0.0.251"));      // mDNS multicast
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("232.0.0.1"));        // SSM multicast
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("239.255.0.1"));      // Admin-scoped multicast
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("239.255.255.255"));  // Highest Class D address

    // 2. Exact boundary neighbors
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("223.255.255.255")); // Immediate lower neighbor (Class C)
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("240.0.0.0"));       // Immediate higher neighbor (Class E)
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("240.0.0.1"));       // Class E
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("255.255.255.255")); // Broadcast

    // 3. Unicast and loopback addresses
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("0.0.0.0"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("127.0.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("10.0.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("172.16.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("192.168.1.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("8.8.8.8"));

    // 4. Adversarial fuzzing: empty, null, whitespace, format corruptions
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress(""));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress(static_cast<const char*>(nullptr)));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress(" 224.0.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224.0.0.1 "));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress(" 224.0.0.1 "));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224.0.0.1\n"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224.0.0.1\t"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224.0.0."));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224.0.0"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224.0.0.1.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("224.0.0.256"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("300.0.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("-1.0.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("multicast.group"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("::1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("ff02::1"));

    // 5. Oversized string buffer overflow attempt
    std::string largeGarbage(2048, '2');
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress(largeGarbage));
}

// ==============================================================================
// 23. Socket Creation, Buffer Ladder, and Invalid Endpoint Fallbacks
// ==============================================================================
TEST_CASE("challenger_m4_socket_buffer_ladder_and_invalid_endpoints") {
    ChallengerWsaGuard wsa;
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    // 1. Invalid IP string must fail gracefully without throwing or crashing
    vban::ReceiverSocketConfig invalidIpCfg;
    invalidIpCfg.listenIp = "999.999.999.999";
    invalidIpCfg.port = 17980;
    EXPECT_FALSE(receiver.start(invalidIpCfg));
    EXPECT_FALSE(receiver.isRunning());

    vban::ReceiverSocketConfig invalidHostCfg;
    invalidHostCfg.listenIp = "not_an_ip";
    invalidHostCfg.port = 17981;
    EXPECT_FALSE(receiver.start(invalidHostCfg));
    EXPECT_FALSE(receiver.isRunning());

    // 2. Buffer Ladder Stress: Requested 0 bytes -> fallback to default (>= 256 KB)
    vban::ReceiverSocketConfig zeroBufCfg;
    zeroBufCfg.listenIp = "127.0.0.1";
    zeroBufCfg.port = 17982;
    zeroBufCfg.socketBufferSize = 0;
    ASSERT_TRUE(receiver.start(zeroBufCfg));
    EXPECT_TRUE(receiver.isRunning());
    ASSERT_GE(receiver.getGrantedBufferSize(), 65536U);
    receiver.stop();
    EXPECT_FALSE(receiver.isRunning());

    // 3. Buffer Ladder Stress: Massive buffer request (64 MB) -> fallback ladder succeeds
    vban::ReceiverSocketConfig hugeBufCfg;
    hugeBufCfg.listenIp = "127.0.0.1";
    hugeBufCfg.port = 17983;
    hugeBufCfg.socketBufferSize = 64 * 1024 * 1024;
    ASSERT_TRUE(receiver.start(hugeBufCfg));
    EXPECT_TRUE(receiver.isRunning());
    ASSERT_GE(receiver.getGrantedBufferSize(), 65536U);
    receiver.stop();
    EXPECT_FALSE(receiver.isRunning());
}

// ==============================================================================
// 24. Loopback UDP High Packet-Rate Ingestion Stress (2,000 Datagrams)
// ==============================================================================
TEST_CASE("challenger_m4_loopback_udp_high_packet_rate_stress") {
    ChallengerWsaGuard wsa;
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    constexpr uint16_t kPort = 17984;
    vban::ReceiverSocketConfig cfg;
    cfg.listenIp = "127.0.0.1";
    cfg.port = kPort;
    cfg.streamName = "ChallengerStream";
    cfg.socketBufferSize = 4 * 1024 * 1024;
    cfg.receiveTimeoutMs = 100;

    ASSERT_TRUE(receiver.start(cfg));
    ASSERT_TRUE(receiver.isRunning());

    // Background consumer draining ring buffer to prevent saturation
    std::atomic<bool> drainRunning{true};
    std::atomic<uint64_t> totalSamplesDrained{0};
    std::thread consumerThread([&]() {
        std::vector<float> drainBuf(1024);
        while (drainRunning.load(std::memory_order_relaxed)) {
            size_t r = ringBuffer.read(drainBuf.data(), drainBuf.size());
            if (r > 0) {
                totalSamplesDrained.fetch_add(r, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        // Final drain
        while (ringBuffer.availableRead() > 0) {
            size_t r = ringBuffer.read(drainBuf.data(), drainBuf.size());
            totalSamplesDrained.fetch_add(r, std::memory_order_relaxed);
        }
    });

    // Sender socket
    challenger_sock_t clientSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(clientSock, CHALLENGER_INVALID_SOCK);

    sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(kPort);
    destAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    constexpr uint32_t kNumPackets = 2000;
    for (uint32_t i = 1; i <= kNumPackets; ++i) {
        auto pkt = makeChallengerPacket(3, 255, 1, vban::VBanDataType::Int16, "ChallengerStream", i);
        int sent = sendto(clientSock, reinterpret_cast<const char*>(pkt.data()),
                          static_cast<int>(pkt.size()), 0,
                          reinterpret_cast<sockaddr*>(&destAddr), sizeof(destAddr));
        ASSERT_EQ(sent, static_cast<int>(pkt.size()));

        // Throttle slightly every 50 packets to simulate standard 3000 pkts/s network flow
        if (i % 50 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    // Give worker thread time to process all packets
    const auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startTime < std::chrono::milliseconds(2000)) {
        auto telem = receiver.getTelemetry();
        if (telem.totalPacketsReceived >= kNumPackets) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    drainRunning.store(false, std::memory_order_release);
    consumerThread.join();
    challenger_close_sock(clientSock);

    auto telem = receiver.getTelemetry();
    receiver.stop();
    EXPECT_FALSE(receiver.isRunning());

    // Assert that almost all or all packets were ingested
    ASSERT_GE(telem.totalPacketsReceived, kNumPackets - 50); // Tolerate minor OS UDP buffer drops
    ASSERT_GE(telem.validPackets, kNumPackets - 50);
    ASSERT_LT(telem.jitterMs, 25.0f); // Loopback jitter must be low
    ASSERT_GT(totalSamplesDrained.load(), 500000U);
}

// ==============================================================================
// 25. Receiver Rapid Start / Stop Lifecycle & Thread Cleanliness Stress
// ==============================================================================
TEST_CASE("challenger_m4_receiver_rapid_start_stop_lifecycle_stress") {
    ChallengerWsaGuard wsa;
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    vban::ReceiverSocketConfig cfg;
    cfg.listenIp = "127.0.0.1";
    cfg.port = 17985;
    cfg.streamName = "Stream1";
    cfg.receiveTimeoutMs = 50;

    // 1. Multiple redundant start calls on running receiver must be safe
    ASSERT_TRUE(receiver.start(cfg));
    EXPECT_TRUE(receiver.isRunning());
    EXPECT_TRUE(receiver.start(cfg)); // Redundant start
    EXPECT_TRUE(receiver.isRunning());

    // 2. Multiple redundant stop calls must be safe
    receiver.stop();
    EXPECT_FALSE(receiver.isRunning());
    receiver.stop(); // Redundant stop
    EXPECT_FALSE(receiver.isRunning());

    // 3. 25 Rapid Start / Stop Cycles (verifying zero thread leak or socket hang)
    for (int cycle = 0; cycle < 25; ++cycle) {
        cfg.port = static_cast<uint16_t>(17986 + (cycle % 10));
        ASSERT_TRUE(receiver.start(cfg));
        EXPECT_TRUE(receiver.isRunning());

        // Alternate immediate stops and small microsecond delays
        if (cycle % 2 == 1) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        receiver.stop();
        EXPECT_FALSE(receiver.isRunning());
    }

    EXPECT_FALSE(receiver.isRunning());
}

// ==============================================================================
// 26. Concurrent Telemetry Snapshot Integrity Under Continuous Traffic
// ==============================================================================
TEST_CASE("challenger_m4_concurrent_telemetry_snapshot_integrity_under_traffic") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    std::atomic<bool> producerRunning{true};
    std::atomic<bool> consistencyViolation{false};
    std::atomic<uint64_t> totalSnapshotsRead{0};

    // Audio consumer draining samples
    std::thread consumer([&]() {
        std::vector<float> drain(512);
        while (producerRunning.load(std::memory_order_relaxed)) {
            ringBuffer.read(drain.data(), 512);
            std::this_thread::yield();
        }
    });

    // Producer simulating high-throughput packet ingestion via processDatagram
    std::thread producer([&]() {
        uint64_t fakeTimeNs = 1000000000ULL;
        for (uint32_t nu = 1; nu <= 10000; ++nu) {
            fakeTimeNs += 5333333ULL; // ~48kHz / 256 samples
            auto pkt = makeChallengerPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", nu);
            receiver.processDatagram(pkt.data(), pkt.size(), fakeTimeNs);

            if (nu % 200 == 0) {
                std::this_thread::yield();
            }
        }
        producerRunning.store(false, std::memory_order_release);
    });

    // 4 Concurrent Reader threads polling getTelemetry()
    constexpr int kNumReaders = 4;
    std::vector<std::thread> readers;
    readers.reserve(kNumReaders);

    for (int r = 0; r < kNumReaders; ++r) {
        readers.emplace_back([&]() {
            while (producerRunning.load(std::memory_order_acquire)) {
                auto telem = receiver.getTelemetry();

                // Telemetry Invariant Assertions:
                if (telem.activeSampleRate != 48000 && telem.activeSampleRate != 0) {
                    consistencyViolation.store(true);
                }
                if (telem.activeChannels != 2 && telem.activeChannels != 0) {
                    consistencyViolation.store(true);
                }
                if (telem.lostPackets > telem.totalPacketsReceived) {
                    consistencyViolation.store(true);
                }
                if (telem.validPackets > telem.totalPacketsReceived) {
                    consistencyViolation.store(true);
                }
                if (!std::isfinite(telem.jitterMs) || telem.jitterMs < 0.0f) {
                    consistencyViolation.store(true);
                }
                if (receiver.getPeakLeft() < 0.0f || receiver.getPeakLeft() > 1.0f) {
                    consistencyViolation.store(true);
                }
                if (receiver.getPeakRight() < 0.0f || receiver.getPeakRight() > 1.0f) {
                    consistencyViolation.store(true);
                }

                totalSnapshotsRead.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    producer.join();
    for (auto& t : readers) {
        t.join();
    }
    consumer.join();

    EXPECT_FALSE(consistencyViolation.load());
    ASSERT_GT(totalSnapshotsRead.load(), 5000U);
    EXPECT_EQ(receiver.getTelemetry().validPackets, 10000U);
    EXPECT_EQ(receiver.getTelemetry().lostPackets, 0U);
}

// ==============================================================================
// 27. Dynamic Stream Reconfiguration Across All Rates, Formats, and Channels
// ==============================================================================
TEST_CASE("challenger_m4_dynamic_reconfiguration_multi_format_stress") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;

    uint32_t reconfigCount = 0;
    uint32_t lastRate = 0;
    uint16_t lastChannels = 0;

    vban::VbanReceiver receiver(ringBuffer, [&](uint32_t sr, uint16_t ch) {
        reconfigCount++;
        lastRate = sr;
        lastChannels = ch;
    });

    struct FormatScenario {
        uint32_t srIndex;
        uint32_t expectedRate;
        uint8_t  nbc;
        uint16_t expectedChannels;
        vban::VBanDataType dataType;
    };

    const std::vector<FormatScenario> scenarios = {
        {3,  48000, 1, 2, vban::VBanDataType::Int16},
        {4,  96000, 1, 2, vban::VBanDataType::Int24},
        {5, 192000, 3, 4, vban::VBanDataType::Float32},
        {21, 768000, 1, 2, vban::VBanDataType::Int32},
        {14, 11025, 0, 1, vban::VBanDataType::Int16},
        {16, 44100, 7, 8, vban::VBanDataType::Float32},
        {3,  48000, 1, 2, vban::VBanDataType::Int16}
    };

    uint32_t nu = 1;
    for (size_t s = 0; s < scenarios.size(); ++s) {
        const auto& sc = scenarios[s];
        auto pkt = makeChallengerPacket(sc.srIndex, 127, sc.nbc, sc.dataType, "Stream1", nu++);

        ASSERT_TRUE(receiver.processDatagram(pkt.data(), pkt.size()));

        if (s == 0) {
            // Initial scenario matches default active rate (48k, 2ch) -> no callback
            EXPECT_EQ(reconfigCount, 0U);
        } else {
            EXPECT_EQ(lastRate, sc.expectedRate);
            EXPECT_EQ(lastChannels, sc.expectedChannels);
        }

        // Drain ring buffer and assert all written samples are finite and normalized [-1.0, 1.0]
        std::vector<float> outSamples(ringBuffer.availableRead());
        if (!outSamples.empty()) {
            size_t r = ringBuffer.read(outSamples.data(), outSamples.size());
            ASSERT_EQ(r, outSamples.size());
            for (float sample : outSamples) {
                ASSERT_TRUE(std::isfinite(sample));
                ASSERT_GE(sample, -1.0f);
                ASSERT_LE(sample, 1.0f);
            }
        }
    }

    EXPECT_EQ(reconfigCount, static_cast<uint32_t>(scenarios.size() - 1));
}

// ==============================================================================
// 28. JNI NativeTelemetry Struct Packing & Binary Layout Verification
// ==============================================================================
TEST_CASE("challenger_m4_jni_struct_packing_and_binary_layout") {
    // 1. Strict size assertion
    ASSERT_EQ(sizeof(vban::NativeTelemetry), 40U);

    // 2. Strict offset assertions
    ASSERT_EQ(offsetof(vban::NativeTelemetry, sampleRate), 0U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, channelCount), 4U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, bitDepth), 6U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, totalPacketsReceived), 8U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, lostPackets), 16U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, jitterMs), 24U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, bufferOccupancyPercent), 28U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, peakLevelLeft), 32U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, peakLevelRight), 36U);

    // 3. Raw byte serialization roundtrip
    vban::NativeTelemetry tOriginal;
    tOriginal.sampleRate = 768000;
    tOriginal.channelCount = 2;
    tOriginal.bitDepth = 32;
    tOriginal.totalPacketsReceived = 12345678901234ULL;
    tOriginal.lostPackets = 42;
    tOriginal.jitterMs = 1.25f;
    tOriginal.bufferOccupancyPercent = 50.0f;
    tOriginal.peakLevelLeft = 0.85f;
    tOriginal.peakLevelRight = 0.90f;

    alignas(8) uint8_t rawBytes[40];
    std::memcpy(rawBytes, &tOriginal, sizeof(tOriginal));

    // Inspect individual binary values from raw buffer
    uint32_t sr = *reinterpret_cast<const uint32_t*>(rawBytes + 0);
    uint16_t ch = *reinterpret_cast<const uint16_t*>(rawBytes + 4);
    uint16_t bd = *reinterpret_cast<const uint16_t*>(rawBytes + 6);
    uint64_t tot = *reinterpret_cast<const uint64_t*>(rawBytes + 8);
    uint64_t lost = *reinterpret_cast<const uint64_t*>(rawBytes + 16);
    float jit = *reinterpret_cast<const float*>(rawBytes + 24);
    float occ = *reinterpret_cast<const float*>(rawBytes + 28);
    float pkL = *reinterpret_cast<const float*>(rawBytes + 32);
    float pkR = *reinterpret_cast<const float*>(rawBytes + 36);

    EXPECT_EQ(sr, 768000U);
    EXPECT_EQ(ch, 2U);
    EXPECT_EQ(bd, 32U);
    EXPECT_EQ(tot, 12345678901234ULL);
    EXPECT_EQ(lost, 42ULL);
    EXPECT_EQ(jit, 1.25f);
    EXPECT_EQ(occ, 50.0f);
    EXPECT_EQ(pkL, 0.85f);
    EXPECT_EQ(pkR, 0.90f);
}

// ==============================================================================
// Milestone 6 Tier 5 Adversarial Coverage Hardening Tests
// ==============================================================================

// 29. Parser NuFrame 32-bit Wraparound, Boundaries & Massive Fuzzing
TEST_CASE("challenger_tier5_parser_nuframe_boundary_and_fuzz") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket outPkt;

    auto makePkt = [](uint32_t nu) {
        return makeChallengerPacket(3, 127, 1, vban::VBanDataType::Int16, "Stream1", nu);
    };

    // 1. Exact 32-bit boundary rollover: 0xFFFFFFFF -> 0x00000000
    auto pMax = makePkt(0xFFFFFFFFU);
    ASSERT_EQ(parser.parse(pMax.data(), pMax.size(), outPkt), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0ULL);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 0ULL);

    auto pZero = makePkt(0x00000000U);
    ASSERT_EQ(parser.parse(pZero.data(), pZero.size(), outPkt), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0ULL);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 0ULL);

    // 2. Rollover with gap across boundary: 0xFFFFFFFE -> 0x00000002 (missing FFFFFFFF, 0, 1 -> 3 lost)
    parser.reset();
    auto pBefore = makePkt(0xFFFFFFFEU);
    ASSERT_EQ(parser.parse(pBefore.data(), pBefore.size(), outPkt), vban::ParseResult::Success);
    auto pAfter = makePkt(0x00000002U);
    ASSERT_EQ(parser.parse(pAfter.data(), pAfter.size(), outPkt), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 3ULL);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 0ULL);

    // 3. Half-space sequence gap boundary (0x7FFFFFFF vs 0x80000000)
    parser.reset();
    auto pStart = makePkt(0);
    ASSERT_EQ(parser.parse(pStart.data(), pStart.size(), outPkt), vban::ParseResult::Success);

    // delta = 0x7FFFFFFF -> forward loss
    auto pHalf = makePkt(0x7FFFFFFFU);
    ASSERT_EQ(parser.parse(pHalf.data(), pHalf.size(), outPkt), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), static_cast<uint64_t>(0x7FFFFFFE));
    ASSERT_EQ(parser.getOutOfOrderPackets(), 0ULL);

    // delta = 0x80000000 -> treated as out-of-order in 32-bit modular sequence arithmetic
    parser.reset();
    ASSERT_EQ(parser.parse(pStart.data(), pStart.size(), outPkt), vban::ParseResult::Success);
    auto pOooThreshold = makePkt(0x80000000U);
    ASSERT_EQ(parser.parse(pOooThreshold.data(), pOooThreshold.size(), outPkt), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0ULL);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 1ULL);

    // 4. Duplicate frame arrival (delta == 0) -> must not advance lastNuFrame or increase lostPackets
    ASSERT_EQ(parser.parse(pStart.data(), pStart.size(), outPkt), vban::ParseResult::Success);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 2ULL);
    ASSERT_EQ(parser.getLostPackets(), 0ULL);

    // 5. Massive sequence fuzzing across uint32_t boundary (5,000 packets)
    parser.reset();
    uint32_t curNu = 0xFFFFF800U;
    auto pInit = makePkt(curNu);
    ASSERT_EQ(parser.parse(pInit.data(), pInit.size(), outPkt), vban::ParseResult::Success);

    uint64_t expectedLoss = 0;
    uint32_t seed = 987654321;
    for (int i = 0; i < 5000; ++i) {
        seed = seed * 1664525 + 1013904223;
        uint32_t step = (seed % 10) + 1; // 1..10 forward step
        uint32_t nextNu = curNu + step;

        auto pFuzz = makePkt(nextNu);
        ASSERT_EQ(parser.parse(pFuzz.data(), pFuzz.size(), outPkt), vban::ParseResult::Success);

        if (step > 1) {
            expectedLoss += (step - 1);
        }
        curNu = nextNu;
    }
    ASSERT_EQ(parser.getLostPackets(), expectedLoss);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 0ULL);
}

// 30. Parser 10-Step Pipeline Malformed Header Fuzzing Matrix
TEST_CASE("challenger_tier5_parser_malformed_10_step_matrix") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket outPkt;

    // Step 1: PacketTooSmall
    for (size_t s = 0; s < 28; ++s) {
        std::vector<uint8_t> small(s, 0);
        ASSERT_EQ(parser.parse(small.data(), small.size(), outPkt), vban::ParseResult::PacketTooSmall);
    }

    auto base = makeChallengerPacket(3, 127, 1, vban::VBanDataType::Int16, "Stream1", 1);

    // Step 2: InvalidMagic
    {
        auto bad = base;
        bad[0] = 'X';
        ASSERT_EQ(parser.parse(bad.data(), bad.size(), outPkt), vban::ParseResult::InvalidMagic);
    }

    // Step 3: UnsupportedSubprotocol (Serial 0x20, Text 0x40, Service 0x60)
    for (uint8_t sp : {0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0}) {
        auto bad = base;
        bad[4] = (bad[4] & 0x1F) | sp;
        ASSERT_EQ(parser.parse(bad.data(), bad.size(), outPkt), vban::ParseResult::UnsupportedSubprotocol);
    }

    // Step 4: UnsupportedCodec (0x10 VBCA, 0x20 VBCV)
    for (uint8_t codec : {0x10, 0x20, 0x30, 0xF0}) {
        auto bad = base;
        bad[7] = (bad[7] & 0x0F) | codec;
        ASSERT_EQ(parser.parse(bad.data(), bad.size(), outPkt), vban::ParseResult::UnsupportedCodec);
    }

    // Step 5: ReservedBitSet (Bit 3)
    {
        auto bad = base;
        bad[7] |= 0x08;
        ASSERT_EQ(parser.parse(bad.data(), bad.size(), outPkt), vban::ParseResult::ReservedBitSet);
    }

    // Step 6: InvalidSampleRate (indices 22..31)
    for (uint8_t sr = 22; sr <= 31; ++sr) {
        auto bad = base;
        bad[4] = (bad[4] & 0xE0) | sr;
        ASSERT_EQ(parser.parse(bad.data(), bad.size(), outPkt), vban::ParseResult::InvalidSampleRate);
    }

    // Step 7: UnsupportedDataType (Float64 0x05, Bits12 0x06, Bits10 0x07)
    for (uint8_t dt : {0x00, 0x05, 0x06, 0x07}) {
        auto bad = base;
        bad[7] = (bad[7] & 0xF8) | dt;
        ASSERT_EQ(parser.parse(bad.data(), bad.size(), outPkt), vban::ParseResult::UnsupportedDataType);
    }

    // Step 8: StreamNameMismatch
    {
        auto bad = base;
        std::memcpy(bad.data() + 8, "WrongStream\0\0\0\0", 16);
        ASSERT_EQ(parser.parse(bad.data(), bad.size(), outPkt), vban::ParseResult::StreamNameMismatch);
    }

    // Step 9: PayloadSizeMismatch
    {
        auto badShort = base;
        badShort.pop_back();
        ASSERT_EQ(parser.parse(badShort.data(), badShort.size(), outPkt), vban::ParseResult::PayloadSizeMismatch);

        auto badLong = base;
        badLong.push_back(0);
        ASSERT_EQ(parser.parse(badLong.data(), badLong.size(), outPkt), vban::ParseResult::PayloadSizeMismatch);
    }

    // Step 9 boundary: maximum allowable format_nbs=255, format_nbc=255, Float32 (262,144 bytes payload)
    {
        size_t maxPayload = vban::calculatePayloadBytes(255, 255, 4);
        ASSERT_EQ(maxPayload, 262144U);
        size_t totalBytes = vban::VBAN_HEADER_SIZE + maxPayload;
        ASSERT_EQ(totalBytes, 262172U);

        // Datagram with valid header declaring max payload but only sending 100 bytes
        std::vector<uint8_t> maxHdr(128, 0);
        auto* hdr = reinterpret_cast<vban::VBanHeader*>(maxHdr.data());
        hdr->magic = vban::VBAN_MAGIC;
        hdr->format_SR = static_cast<uint8_t>(vban::VBanSubProtocol::Audio) | 21; // 768 kHz
        hdr->format_nbs = 255;
        hdr->format_nbc = 255;
        hdr->format_bit = static_cast<uint8_t>(vban::VBanCodec::PCM) | static_cast<uint8_t>(vban::VBanDataType::Float32);
        std::memcpy(hdr->streamName, "Stream1", 7);
        hdr->nuFrame = 1;

        ASSERT_EQ(parser.parse(maxHdr.data(), maxHdr.size(), outPkt), vban::ParseResult::PayloadSizeMismatch);
    }
}

// 31. Converters Subnormals, Denormals, IEEE-754 Sanitization & Math Oracles
TEST_CASE("challenger_tier5_converters_subnormals_and_precision") {
    // 1. Float32 IEEE-754 NaNs, Infs, and subnormals
    constexpr size_t kSampleCount = 10;
    alignas(16) uint32_t rawBits[kSampleCount] = {
        0x7FC00000U, // Quiet NaN
        0x7F800001U, // Signaling NaN
        0x7F800000U, // +Infinity
        0xFF800000U, // -Infinity
        0x00000001U, // Smallest positive denormal (~1.4e-45)
        0x80000001U, // Smallest negative denormal (~-1.4e-45)
        0x3F800000U, // +1.0f
        0xBF800000U, // -1.0f
        0x40A00000U, // +5.0f (must clamp to +1.0f)
        0xC1200000U  // -10.0f (must clamp to -1.0f)
    };

    alignas(16) float sanitized[kSampleCount];
    vban::convertFloat32Sanitized(reinterpret_cast<const uint8_t*>(rawBits), sanitized, kSampleCount);

    EXPECT_EQ(sanitized[0], 0.0f);
    EXPECT_EQ(sanitized[1], 0.0f);
    EXPECT_EQ(sanitized[2], 0.0f);
    EXPECT_EQ(sanitized[3], 0.0f);
    EXPECT_TRUE(std::isfinite(sanitized[4]));
    ASSERT_GE(sanitized[4], -1.0f);
    ASSERT_LE(sanitized[4], 1.0f);
    EXPECT_TRUE(std::isfinite(sanitized[5]));
    ASSERT_GE(sanitized[5], -1.0f);
    ASSERT_LE(sanitized[5], 1.0f);
    EXPECT_NEAR(sanitized[6], 1.0f, 1e-7f);
    EXPECT_NEAR(sanitized[7], -1.0f, 1e-7f);
    EXPECT_EQ(sanitized[8], 1.0f);  // Clamped
    EXPECT_EQ(sanitized[9], -1.0f); // Clamped

    // 2. 24-bit PCM packed sign extension oracle
    const uint8_t bytes24[18] = {
        0xFF, 0xFF, 0x7F, // 0x7FFFFF (+8,388,607)
        0x00, 0x00, 0x80, // 0x800000 (-8,388,608)
        0x01, 0x00, 0x80, // 0x800001 (-8,388,607)
        0xFF, 0xFF, 0xFF, // 0xFFFFFF (-1)
        0x00, 0x00, 0x00, // 0x000000 (0)
        0x01, 0x00, 0x00  // 0x000001 (+1)
    };
    float out24[6];
    vban::convertInt24ToFloat(bytes24, out24, 6);

    EXPECT_NEAR(out24[0], 8388607.0f / 8388608.0f, 1e-7f);
    EXPECT_NEAR(out24[1], -1.0f, 1e-7f);
    EXPECT_NEAR(out24[2], -8388607.0f / 8388608.0f, 1e-7f);
    EXPECT_NEAR(out24[3], -1.0f / 8388608.0f, 1e-7f);
    EXPECT_NEAR(out24[4], 0.0f, 1e-7f);
    EXPECT_NEAR(out24[5], 1.0f / 8388608.0f, 1e-7f);

    // 3. 16-bit PCM limits
    int16_t in16[3] = {-32768, 32767, 0};
    float out16[3];
    vban::convertInt16ToFloat(reinterpret_cast<const uint8_t*>(in16), out16, 3);
    EXPECT_NEAR(out16[0], -1.0f, 1e-7f);
    EXPECT_NEAR(out16[1], 32767.0f / 32768.0f, 1e-7f);
    EXPECT_NEAR(out16[2], 0.0f, 1e-7f);

    // 4. 32-bit PCM limits
    int32_t in32[3] = {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), 0};
    float out32[3];
    vban::convertInt32ToFloat(reinterpret_cast<const uint8_t*>(in32), out32, 3);
    EXPECT_NEAR(out32[0], -1.0f, 1e-7f);
    EXPECT_NEAR(out32[1], 2147483647.0f / 2147483648.0f, 1e-7f);
    EXPECT_NEAR(out32[2], 0.0f, 1e-7f);
}

// 32. SPSC Ring Buffer Whole-Frame Saturation and Alignment Invariant
TEST_CASE("challenger_tier5_spsc_whole_frame_saturation_stress") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    // Test with 3-channel, 5-channel, 7-channel, and 8-channel formats
    for (uint16_t ch : {3, 5, 7, 8}) {
        ringBuffer.reset();
        uint8_t nbc = static_cast<uint8_t>(ch - 1);
        uint8_t nbs = 127; // 128 frames

        // 1. Process datagrams until ring buffer is nearly full
        size_t framesPerPacket = 128;
        size_t samplesPerPacket = framesPerPacket * ch;
        size_t packetsToFill = (vban::VBAN_RING_BUFFER_CAPACITY / samplesPerPacket);

        uint32_t nu = 1;
        for (size_t p = 0; p < packetsToFill; ++p) {
            auto pkt = makeChallengerPacket(3, nbs, nbc, vban::VBanDataType::Int16, "Stream1", nu++);
            ASSERT_TRUE(receiver.processDatagram(pkt.data(), pkt.size()));
        }

        // Available write space may be 0 or less than a full packet
        size_t freeSamples = ringBuffer.availableWrite();
        ASSERT_GE(freeSamples, 0U);

        // 2. Ingest one more packet that partially exceeds capacity
        auto pktOverflow = makeChallengerPacket(3, nbs, nbc, vban::VBanDataType::Int16, "Stream1", nu++);
        ASSERT_TRUE(receiver.processDatagram(pktOverflow.data(), pktOverflow.size()));

        // Invariant: Total elements in buffer MUST be an exact multiple of channel count ch!
        size_t elementsInBuffer = ringBuffer.availableRead();
        ASSERT_EQ(elementsInBuffer % ch, 0U);

        // Verify receiver telemetry reports dropped frames without channel desync
        auto telem = receiver.getTelemetry();
        ASSERT_GT(telem.overflowFrames, 0ULL);
    }
}

// 33. High-Rate 768 kHz Catmull-Rom Micro-Resampling & Raised Cosine Fade Invariants
TEST_CASE("challenger_tier5_high_rate_768khz_drift_and_fade_invariants") {
    // 1. Raised Cosine window exact symmetry: w(n) + w(128 - n) == 1.0f
    for (size_t n = 0; n <= vban::FadeEngine::FADE_WINDOW_SIZE; ++n) {
        float wn = vban::FadeEngine::getCosineWindow(n);
        float w_sym = vban::FadeEngine::getCosineWindow(vban::FadeEngine::FADE_WINDOW_SIZE - n);
        EXPECT_NEAR(wn + w_sym, 1.0f, 1e-6f);
    }

    // 2. Rapid fade reversals at sample indices 1, 63, 64, 65, 127
    vban::FadeEngine engine;
    engine.reset();
    engine.startFadeIn();
    std::vector<float> testAudio(256 * 2, 0.5f);

    engine.process(testAudio.data(), 40, 2);
    EXPECT_EQ(engine.getState(), vban::FadeState::FadingIn);

    // Reverse to fade-out mid-stride
    engine.startFadeOut();
    EXPECT_EQ(engine.getState(), vban::FadeState::FadingOut);
    engine.process(testAudio.data(), 20, 2);

    // Reverse back to fade-in
    engine.startFadeIn();
    EXPECT_EQ(engine.getState(), vban::FadeState::FadingIn);
    engine.process(testAudio.data(), 128, 2);
    EXPECT_EQ(engine.getState(), vban::FadeState::Active);

    // 3. 768 kHz high-throughput Catmull-Rom resampler accuracy
    vban::CatmullRomResampler resampler;
    resampler.reset();

    constexpr size_t kChannels = 2;
    constexpr size_t kInputFrames = 2048;
    constexpr size_t kOutputFrames = 1024;
    std::vector<float> input(kInputFrames * kChannels);

    // Generate pure 1 kHz tone at 768 kHz sample rate
    constexpr double kRate = 768000.0;
    constexpr double kFreq = 1000.0;
    constexpr double kPiVal = 3.14159265358979323846;
    for (size_t f = 0; f < kInputFrames; ++f) {
        float s = static_cast<float>(std::sin(2.0 * kPiVal * kFreq * (f / kRate)));
        input[f * 2 + 0] = s;
        input[f * 2 + 1] = s;
    }

    std::vector<float> output(kOutputFrames * kChannels);
    size_t consumed = 0;
    // Resample with +100 PPM ratio (1.000100)
    size_t produced = resampler.processDirect(
        input.data(), kInputFrames, consumed,
        output.data(), kOutputFrames, kChannels,
        1.000100
    );

    ASSERT_EQ(produced, kOutputFrames);
    ASSERT_GT(consumed, 0U);

    // Verify all output samples are finite, continuous and bounded within [-1.0f, +1.0f]
    for (size_t i = 0; i < kOutputFrames * kChannels; ++i) {
        ASSERT_TRUE(std::isfinite(output[i]));
        ASSERT_GE(output[i], -1.0f);
        ASSERT_LE(output[i], 1.0f);
    }
}

// 34. End-to-End Multi-Threaded Native Pipeline Stress (Ingest -> SPSC -> Resample -> Telemetry)
TEST_CASE("challenger_tier5_concurrent_pipeline_telemetry_stress") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::ClockDriftCompensator compensator;
    vban::VbanReceiver receiver(ringBuffer);

    std::atomic<bool> stopFlag{false};
    std::atomic<uint64_t> packetsIngested{0};
    std::atomic<uint64_t> framesResampled{0};
    std::atomic<uint64_t> telemetryReads{0};

    // Thread 1: Ingest 768 kHz packets into VbanReceiver
    std::thread ingester([&]() {
        uint32_t nu = 1;
        while (!stopFlag.load(std::memory_order_relaxed)) {
            auto pkt = makeChallengerPacket(21, 63, 1, vban::VBanDataType::Int16, "Stream1", nu++);
            receiver.processDatagram(pkt.data(), pkt.size());
            packetsIngested.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Thread 2: Consumer callback pulling through compensator / resampler
    std::thread consumer([&]() {
        std::vector<float> cbBuffer(256 * 2);
        while (!stopFlag.load(std::memory_order_relaxed)) {
            size_t produced = compensator.process(ringBuffer, cbBuffer.data(), 256, 2);
            framesResampled.fetch_add(produced, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Thread 3: Concurrently poll telemetry snapshots
    std::thread poller([&]() {
        while (!stopFlag.load(std::memory_order_relaxed)) {
            auto rxTelem = receiver.getTelemetry();
            auto compTelem = compensator.getTelemetry();
            float occ = ringBuffer.occupancyPercent();

            ASSERT_GE(rxTelem.totalPacketsReceived, 0ULL);
            ASSERT_GE(compTelem.currentRatio, 0.5f);
            ASSERT_LE(compTelem.currentRatio, 2.0f);
            ASSERT_GE(occ, 0.0f);
            ASSERT_LE(occ, 100.0f);

            telemetryReads.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Run pipeline for 150 milliseconds
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stopFlag.store(true, std::memory_order_relaxed);

    ingester.join();
    consumer.join();
    poller.join();

    ASSERT_GT(packetsIngested.load(), 100ULL);
    ASSERT_GT(framesResampled.load(), 500ULL);
    ASSERT_GT(telemetryReads.load(), 50ULL);
}

