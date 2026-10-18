#include "test_framework.hpp"
#include "vban_protocol.hpp"
#include "vban_parser.hpp"

#include <vector>
#include <cstring>
#include <cmath>

namespace {

/**
 * Helper generating a valid dummy VBAN packet for unit testing.
 */
std::vector<uint8_t> createVbanPacket(
    uint32_t srIndex = 3,       // Default 48 kHz
    uint8_t  nbs = 255,         // Default 256 samples
    uint8_t  nbc = 1,           // Default 2 channels (stereo)
    vban::VBanDataType dt = vban::VBanDataType::Int16,
    const char* streamName = "Stream1",
    uint32_t nuFrame = 0) {

    uint8_t sampleSize = vban::getSampleSize(dt);
    size_t payloadBytes = vban::calculatePayloadBytes(nbs, nbc, sampleSize);
    std::vector<uint8_t> packet(vban::VBAN_HEADER_SIZE + payloadBytes, 0);

    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
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

    return packet;
}

} // anonymous namespace

// ==============================================================================
// 1. Structure Layout & Size Tests
// ==============================================================================

TEST_CASE("vban_header_size_and_field_offsets") {
    ASSERT_EQ(sizeof(vban::VBanHeader), 28U);
    ASSERT_EQ(offsetof(vban::VBanHeader, magic), 0U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_SR), 4U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_nbs), 5U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_nbc), 6U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_bit), 7U);
    ASSERT_EQ(offsetof(vban::VBanHeader, streamName), 8U);
    ASSERT_EQ(offsetof(vban::VBanHeader, nuFrame), 24U);
}

TEST_CASE("vban_magic_constant") {
    ASSERT_EQ(vban::VBAN_MAGIC, 0x4E414256U);
    
    // Check ASCII byte ordering
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&vban::VBAN_MAGIC);
    ASSERT_EQ(bytes[0], 'V');
    ASSERT_EQ(bytes[1], 'B');
    ASSERT_EQ(bytes[2], 'A');
    ASSERT_EQ(bytes[3], 'N');
}

// ==============================================================================
// 2. Sample Rate Table & Extensions
// ==============================================================================

TEST_CASE("vban_sr_table_all_21_official_rates") {
    ASSERT_EQ(vban::VBAN_OFFICIAL_SR_COUNT, 21U);
    
    // 6 kHz family
    ASSERT_EQ(vban::VBAN_SR_TABLE[0], 6000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[1], 12000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[2], 24000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[3], 48000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[4], 96000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[5], 192000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[6], 384000U);

    // 8 kHz family
    ASSERT_EQ(vban::VBAN_SR_TABLE[7], 8000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[8], 16000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[9], 32000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[10], 64000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[11], 128000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[12], 256000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[13], 512000U);

    // 11.025 kHz family
    ASSERT_EQ(vban::VBAN_SR_TABLE[14], 11025U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[15], 22050U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[16], 44100U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[17], 88200U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[18], 176400U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[19], 352800U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[20], 705600U);
}

TEST_CASE("vban_sr_table_768khz_extension") {
    ASSERT_EQ(vban::VBAN_EXTENDED_SR_COUNT, 22U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[21], 768000U);

    // Helper functions
    auto rate21 = vban::getSampleRate(21);
    ASSERT_TRUE(rate21.has_value());
    ASSERT_EQ(*rate21, 768000U);

    // Out of range indices
    auto rate22 = vban::getSampleRate(22);
    ASSERT_FALSE(rate22.has_value());

    auto rate31 = vban::getSampleRate(31);
    ASSERT_FALSE(rate31.has_value());
}

// ==============================================================================
// 3. Data Types & Sample Sizes
// ==============================================================================

TEST_CASE("vban_data_types_and_sample_sizes") {
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Byte8), 1U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Int16), 2U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Int24), 3U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Int32), 4U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Float32), 4U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Float64), 8U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Bits12), 2U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Bits10), 2U);
}

TEST_CASE("vban_payload_calculation") {
    // 1 sample, 1 ch, 16-bit
    ASSERT_EQ(vban::calculatePayloadBytes(0, 0, 2), 2U);
    ASSERT_EQ(vban::calculateTotalPacketBytes(0, 0, 2), 30U);

    // 256 samples, 2 ch, 16-bit
    ASSERT_EQ(vban::calculatePayloadBytes(255, 1, 2), 1024U);
    ASSERT_EQ(vban::calculateTotalPacketBytes(255, 1, 2), 1052U);

    // 128 samples, 8 ch, 32-bit
    ASSERT_EQ(vban::calculatePayloadBytes(127, 7, 4), 4096U);
    ASSERT_EQ(vban::calculateTotalPacketBytes(127, 7, 4), 4124U);
}

// ==============================================================================
// 4. Stream Name Extraction & Trimming
// ==============================================================================

TEST_CASE("vban_stream_name_extraction_trimming") {
    char name1[16] = {'M', 'a', 'i', 'n', '\0', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_EQ(vban::extractStreamName(name1), "Main");

    char name2[16] = {'S', 't', 'r', 'e', 'a', 'm', '1', ' ', ' ', ' ', 0, 0, 0, 0, 0, 0};
    ASSERT_EQ(vban::extractStreamName(name2), "Stream1");

    char name3[16] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6'};
    ASSERT_EQ(vban::extractStreamName(name3), "1234567890123456");
}

// ==============================================================================
// 5. 10-Step Validation Pipeline Tests
// ==============================================================================

TEST_CASE("vban_parser_step1_packet_too_small") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    ASSERT_EQ(parser.parse(nullptr, 0, out), vban::ParseResult::PacketTooSmall);

    uint8_t tiny[27] = {0};
    ASSERT_EQ(parser.parse(tiny, sizeof(tiny), out), vban::ParseResult::PacketTooSmall);
}

TEST_CASE("vban_parser_step2_invalid_magic") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    hdr->magic = 0x54534554U; // 'TEST'

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::InvalidMagic);
}

TEST_CASE("vban_parser_step3_unsupported_subprotocol") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // Serial subprotocol (0x20)
    hdr->format_SR = static_cast<uint8_t>(vban::VBanSubProtocol::Serial) | 0x03;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::UnsupportedSubprotocol);

    // Text subprotocol (0x40)
    hdr->format_SR = static_cast<uint8_t>(vban::VBanSubProtocol::Text) | 0x03;
    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::UnsupportedSubprotocol);
}

TEST_CASE("vban_parser_step4_unsupported_codec") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // VBCA compressed codec (0x10)
    hdr->format_bit = static_cast<uint8_t>(vban::VBanCodec::VBCA) | 0x01;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::UnsupportedCodec);
}

TEST_CASE("vban_parser_step5_reserved_bit3_check") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // Set Bit 3 (0x08)
    hdr->format_bit |= vban::VBAN_RESERVED_BIT3_MASK;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::ReservedBitSet);
}

TEST_CASE("vban_parser_step6_invalid_sample_rate") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // SR Index 22 (invalid)
    hdr->format_SR = (hdr->format_SR & 0xE0) | 22;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::InvalidSampleRate);
}

TEST_CASE("vban_parser_step7_unsupported_data_type") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Byte8 (0x00)
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Byte8);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::UnsupportedDataType);

    // Float64 (0x05)
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Float64);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::UnsupportedDataType);
}

TEST_CASE("vban_parser_step8_stream_name_filtering") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Matching stream
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1");
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::Success);

    // Case-insensitive match ("stream1" matches "Stream1")
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "stream1");
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::Success);

    // Mismatched stream
    auto p3 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream2");
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::StreamNameMismatch);

    // Wildcard mode (empty target accepts everything)
    parser.setTargetStreamName("");
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::Success);
}

TEST_CASE("vban_parser_step9_payload_size_mismatch") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16); // Expected 1052 bytes
    ASSERT_EQ(packet.size(), 1052U);

    // Truncated packet (-1 byte)
    ASSERT_EQ(parser.parse(packet.data(), packet.size() - 1, out), vban::ParseResult::PayloadSizeMismatch);

    // Packet with trailing garbage (+4 bytes)
    packet.resize(packet.size() + 4);
    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::PayloadSizeMismatch);
}

TEST_CASE("vban_parser_valid_packet_and_metadata") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 42);
    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::Success);

    ASSERT_EQ(out.sampleRate, 48000U);
    ASSERT_EQ(out.channels, 2U);
    ASSERT_EQ(out.samplesPerFrame, 256U);
    ASSERT_EQ(out.sampleSize, 2U);
    ASSERT_EQ(out.payloadBytes, 1024U);
    ASSERT_EQ(out.dataType, vban::VBanDataType::Int16);
    ASSERT_EQ(out.streamName, "Stream1");
    ASSERT_EQ(out.frameNumber, 42U);
    ASSERT_EQ(out.payload, packet.data() + 28);
}

// ==============================================================================
// 6. Sequence Number Loss Tracking & Wrap Tests
// ==============================================================================

TEST_CASE("vban_parser_sequence_loss_tracking") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Packet 0: frame 100
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 100);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Packet 1: frame 101 (delta 1, no loss)
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 101);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Packet 2: frame 105 (delta 4, 3 lost)
    auto p3 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 105);
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 3U);

    // Packet 3: frame 103 (out of order)
    auto p4 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 103);
    ASSERT_EQ(parser.parse(p4.data(), p4.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 3U);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 1U);
}

TEST_CASE("vban_parser_sequence_wrap_around") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Frame 0xFFFFFFFF
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0xFFFFFFFFU);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Frame 0x00000000 (wraps over, delta == 1)
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0x00000000U);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Frame 0x00000005 (delta == 5, 4 lost)
    auto p3 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0x00000005U);
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 4U);
}

// ==============================================================================
// 7. RFC 3550 Inter-Arrival Jitter Calculation
// ==============================================================================

TEST_CASE("vban_parser_rfc3550_jitter_ideal_arrival") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // 48 kHz, 256 samples/packet -> nominal interval = 256 / 48000 = 5.333333 ms
    constexpr uint64_t intervalNs = 5333333ULL;
    uint64_t timeNs = 1000000000ULL;

    for (uint32_t frame = 0; frame < 10; ++frame) {
        auto p = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", frame);
        ASSERT_EQ(parser.parse(p.data(), p.size(), timeNs, out), vban::ParseResult::Success);
        timeNs += intervalNs;
    }

    // Jitter with perfect arrivals must remain close to 0
    ASSERT_NEAR(parser.getJitterMs(), 0.0, 0.001);
}

TEST_CASE("vban_parser_rfc3550_jitter_filter_convergence") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    constexpr uint64_t nominalNs = 5333333ULL;
    uint64_t timeNs = 1000000000ULL;

    // Packet 0
    auto p0 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0);
    ASSERT_EQ(parser.parse(p0.data(), p0.size(), timeNs, out), vban::ParseResult::Success);
    ASSERT_NEAR(parser.getJitterMs(), 0.0, 1e-6);

    // Packet 1 delayed by 2.0 ms (+2,000,000 ns)
    timeNs += nominalNs + 2000000ULL;
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 1);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), timeNs, out), vban::ParseResult::Success);

    // D = 2.0 ms. Jitter = 0 + (2.0 - 0) / 16.0 = 0.125 ms
    ASSERT_NEAR(parser.getJitterMs(), 0.125, 0.001);

    // Packet 2 arrives on schedule (+nominalNs)
    timeNs += nominalNs;
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 2);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), timeNs, out), vban::ParseResult::Success);

    // D = 0.0 ms. Jitter = 0.125 + (0.0 - 0.125) / 16.0 = 0.1171875 ms
    ASSERT_NEAR(parser.getJitterMs(), 0.1171875, 0.001);
}
