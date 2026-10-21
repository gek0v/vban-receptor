#include "test_framework.hpp"
#include "vban_protocol.hpp"
#include "vban_parser.hpp"
#include "vban_converter.hpp"
#include "spsc_ring_buffer.hpp"
#include "vban_receiver.hpp"

#include <vector>
#include <array>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cmath>

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
using socket_t = SOCKET;
constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
inline int close_sock(socket_t s) { return closesocket(s); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCK = -1;
inline int close_sock(socket_t s) { return close(s); }
#endif

namespace {

/**
 * RAII Initializer for Windows Winsock.
 */
struct WsaGuard {
    WsaGuard() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~WsaGuard() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

/**
 * Helper to construct a valid VBAN audio datagram for loopback and pipeline testing.
 */
std::vector<uint8_t> createTestVbanPacket(
    uint32_t srIndex = 3,       // 48 kHz
    uint8_t  nbs = 255,         // 256 samples
    uint8_t  nbc = 1,           // 2 channels (stereo)
    vban::VBanDataType dt = vban::VBanDataType::Int16,
    const char* streamName = "Stream1",
    uint32_t nuFrame = 1) {

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

    // Fill payload with a 1 kHz test sine wave in Int16
    if (dt == vban::VBanDataType::Int16) {
        auto* samples = reinterpret_cast<int16_t*>(packet.data() + vban::VBAN_HEADER_SIZE);
        size_t totalSamples = (nbs + 1) * (nbc + 1);
        for (size_t i = 0; i < totalSamples; ++i) {
            samples[i] = static_cast<int16_t>(16384.0 * std::sin(2.0 * 3.1415926535 * i / 48.0));
        }
    }

    return packet;
}

} // anonymous namespace

// ==============================================================================
// 1. UDP Socket Creation, 4MB SO_RCVBUF, and Teardown Tests
// ==============================================================================

TEST_CASE("receiver_socket_create_bind_and_buffer_config") {
    WsaGuard wsa;

    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(sock, INVALID_SOCK);

    // 1. Configure SO_REUSEADDR
    int optTrue = 1;
    int res = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optTrue), sizeof(optTrue));
    EXPECT_EQ(res, 0);

    // 2. Request 4 MB SO_RCVBUF (4 * 1024 * 1024 = 4,194,304 bytes)
    int reqBuffer = 4 * 1024 * 1024;
    res = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&reqBuffer), sizeof(reqBuffer));
    EXPECT_EQ(res, 0);

    // 3. Query back SO_RCVBUF
    int actualBuffer = 0;
#ifdef _WIN32
    int optLen = sizeof(actualBuffer);
#else
    socklen_t optLen = sizeof(actualBuffer);
#endif
    res = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&actualBuffer), &optLen);
    EXPECT_EQ(res, 0);
    ASSERT_GT(actualBuffer, 65536);

    // 4. Configure SO_RCVTIMEO (100 ms)
#ifdef _WIN32
    DWORD timeoutMs = 100;
    res = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    struct timeval tv{0, 100000};
    res = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const void*>(&tv), sizeof(tv));
#endif
    EXPECT_EQ(res, 0);

    // 5. Bind to loopback ephemeral port
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0); // Ephemeral port
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    res = bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ASSERT_EQ(res, 0);

    // Clean teardown
    close_sock(sock);
}

TEST_CASE("receiver_bind_fallback_to_inaddr_any_on_remote_transmitter_ip") {
    WsaGuard wsa;
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    vban::ReceiverSocketConfig cfg;
    // 192.0.2.1 is TEST-NET-1 (RFC 5737), not assigned to any local interface
    cfg.listenIp = "192.0.2.1";
    cfg.port = 18985;
    cfg.streamName = "FallbackTest";
    cfg.socketBufferSize = 65536;

    // Should gracefully fall back to INADDR_ANY and bind successfully
    ASSERT_TRUE(receiver.start(cfg));
    EXPECT_TRUE(receiver.isRunning());
    receiver.stop();
    EXPECT_FALSE(receiver.isRunning());
}

// ==============================================================================
// 2. Multicast Address Validation Tests
// ==============================================================================

TEST_CASE("receiver_multicast_address_range_validation") {
    // Valid IPv4 Multicast addresses (224.0.0.0 to 239.255.255.255)
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("224.0.0.1"));
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("224.0.0.251"));
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("239.255.0.1"));
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("239.255.255.255"));
    EXPECT_TRUE(vban::VbanReceiver::isMulticastAddress("232.0.1.2"));

    // Non-multicast unicast / loopback / broadcast addresses
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("127.0.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("192.168.1.100"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("10.0.0.1"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("0.0.0.0"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("223.255.255.255")); // Right below multicast range
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("240.0.0.1"));       // Right above multicast range
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("255.255.255.255")); // Limited broadcast

    // Malformed IP strings
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress(""));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("invalid_ip"));
    EXPECT_FALSE(vban::VbanReceiver::isMulticastAddress("999.999.999.999"));
}

// ==============================================================================
// 3. Loopback Packet Sending and Receiving Tests
// ==============================================================================

TEST_CASE("receiver_loopback_udp_transmission_and_reception") {
    WsaGuard wsa;

    // Server socket (receiver side)
    socket_t serverSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(serverSock, INVALID_SOCK);

    int optTrue = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optTrue), sizeof(optTrue));

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(0); // Ephemeral
    serverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ASSERT_EQ(bind(serverSock, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)), 0);

    // Retrieve dynamically assigned port
#ifdef _WIN32
    int len = sizeof(serverAddr);
#else
    socklen_t len = sizeof(serverAddr);
#endif
    ASSERT_EQ(getsockname(serverSock, reinterpret_cast<struct sockaddr*>(&serverAddr), &len), 0);
    uint16_t assignedPort = ntohs(serverAddr.sin_port);
    ASSERT_GT(assignedPort, 0);

    // Set receive timeout
#ifdef _WIN32
    DWORD timeout = 500;
    setsockopt(serverSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv{0, 500000};
    setsockopt(serverSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const void*>(&tv), sizeof(tv));
#endif

    // Client socket (sender side)
    socket_t clientSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(clientSock, INVALID_SOCK);

    auto testPacket = createTestVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 100);

    struct sockaddr_in destAddr{};
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(assignedPort);
    destAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int sent = sendto(clientSock, reinterpret_cast<const char*>(testPacket.data()),
                      static_cast<int>(testPacket.size()), 0,
                      reinterpret_cast<struct sockaddr*>(&destAddr), sizeof(destAddr));
    ASSERT_EQ(sent, static_cast<int>(testPacket.size()));

    // Receive on server socket
    std::vector<uint8_t> recvBuf(4096, 0);
    int received = recvfrom(serverSock, reinterpret_cast<char*>(recvBuf.data()),
                            static_cast<int>(recvBuf.size()), 0, nullptr, nullptr);
    ASSERT_EQ(received, static_cast<int>(testPacket.size()));

    // Verify received datagram parses correctly
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket parsed;
    vban::ParseResult res = parser.parse(recvBuf.data(), received, parsed);
    ASSERT_EQ(res, vban::ParseResult::Success);
    EXPECT_EQ(parsed.sampleRate, 48000U);
    EXPECT_EQ(parsed.channels, 2U);
    EXPECT_EQ(parsed.samplesPerFrame, 256U);
    EXPECT_EQ(parsed.frameNumber, 100U);

    close_sock(clientSock);
    close_sock(serverSock);
}

// ==============================================================================
// 4. Receiver Pipeline Datagram Ingestion and SPSC Push
// ==============================================================================

TEST_CASE("receiver_pipeline_datagram_ingestion_and_spsc_push") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    auto testPacket = createTestVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 1);

    bool ok = receiver.processDatagram(testPacket.data(), testPacket.size());
    ASSERT_TRUE(ok);

    // 256 frames * 2 channels = 512 samples
    EXPECT_EQ(ringBuffer.availableRead(), 512U);

    std::vector<float> readSamples(512, 0.0f);
    size_t readCount = ringBuffer.read(readSamples.data(), 512);
    EXPECT_EQ(readCount, 512U);

    // Verify samples are non-zero and within [-1.0f, +1.0f]
    float maxVal = 0.0f;
    for (float s : readSamples) {
        float absVal = std::abs(s);
        if (absVal > maxVal) maxVal = absVal;
        ASSERT_LE(absVal, 1.0f);
    }
    ASSERT_GT(maxVal, 0.1f);

    // Verify telemetry
    auto telem = receiver.getTelemetry();
    EXPECT_EQ(telem.totalPacketsReceived, 1U);
    EXPECT_EQ(telem.validPackets, 1U);
    EXPECT_EQ(telem.lostPackets, 0U);
    ASSERT_GT(receiver.getPeakLeft(), 0.1f);
    ASSERT_GT(receiver.getPeakRight(), 0.1f);
}

// ==============================================================================
// 5. Dynamic Stream Reconfiguration Trigger
// ==============================================================================

TEST_CASE("receiver_dynamic_stream_reconfiguration_trigger") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;

    uint32_t reconfigCount = 0;
    uint32_t lastReportedRate = 0;
    uint16_t lastReportedChannels = 0;

    vban::VbanReceiver receiver(ringBuffer, [&](uint32_t sr, uint16_t ch) {
        reconfigCount++;
        lastReportedRate = sr;
        lastReportedChannels = ch;
    });

    // 1. Initial stream packet: 48 kHz (index 3), Stereo
    auto packet48k = createTestVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 1);
    ASSERT_TRUE(receiver.processDatagram(packet48k.data(), packet48k.size()));
    // Initial active rate is 48000, so reconfig count remains 0
    EXPECT_EQ(reconfigCount, 0U);

    // 2. Stream switches to 96 kHz (index 4), Stereo
    auto packet96k = createTestVbanPacket(4, 255, 1, vban::VBanDataType::Int16, "Stream1", 2);
    ASSERT_TRUE(receiver.processDatagram(packet96k.data(), packet96k.size()));
    EXPECT_EQ(reconfigCount, 1U);
    EXPECT_EQ(lastReportedRate, 96000U);
    EXPECT_EQ(lastReportedChannels, 2U);

    // 3. Second 96 kHz packet should NOT trigger additional reconfig
    auto packet96k_2 = createTestVbanPacket(4, 255, 1, vban::VBanDataType::Int16, "Stream1", 3);
    ASSERT_TRUE(receiver.processDatagram(packet96k_2.data(), packet96k_2.size()));
    EXPECT_EQ(reconfigCount, 1U);

    // 4. Stream switches to 4 channels at 96 kHz
    auto packet96k_4ch = createTestVbanPacket(4, 255, 3, vban::VBanDataType::Int16, "Stream1", 4);
    ASSERT_TRUE(receiver.processDatagram(packet96k_4ch.data(), packet96k_4ch.size()));
    EXPECT_EQ(reconfigCount, 2U);
    EXPECT_EQ(lastReportedRate, 96000U);
    EXPECT_EQ(lastReportedChannels, 4U);
}

// ==============================================================================
// 6. JNI NativeTelemetry Struct Packing & Non-Blocking Snapshot Integrity
// ==============================================================================

TEST_CASE("jni_telemetry_struct_packing_and_alignment") {
    // Verify strict field sizes and offsets matching the JNI constructor:
    // (sampleRate: I, channelCount: I, bitDepth: I, totalPackets: J, lostPackets: J,
    //  jitterMs: F, bufferOccupancy: F, peakL: F, peakR: F, targetWatermarkMs: F,
    //  unrecoverableDrops: J, glitchCount: J) -> V

    ASSERT_EQ(sizeof(vban::NativeTelemetry), 64U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, sampleRate), 0U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, channelCount), 4U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, bitDepth), 6U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, totalPacketsReceived), 8U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, lostPackets), 16U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, jitterMs), 24U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, bufferOccupancyPercent), 28U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, peakLevelLeft), 32U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, peakLevelRight), 36U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, targetWatermarkMs), 40U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, unrecoverableDrops), 48U);
    ASSERT_EQ(offsetof(vban::NativeTelemetry, glitchCount), 56U);
}

TEST_CASE("jni_telemetry_concurrent_snapshot_integrity") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    std::atomic<bool> writerDone{false};
    std::atomic<uint64_t> readSnapshots{0};
    std::atomic<bool> consistencyViolation{false};

    // Writer thread feeding live VBAN packets into VbanReceiver
    std::thread writer([&]() {
        uint32_t nuFrame = 1;
        // Guarantee writer keeps producing until reader completes at least 200 snapshots
        while (readSnapshots.load(std::memory_order_relaxed) < 200 || nuFrame < 1000) {
            auto packet = createTestVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", nuFrame++);
            receiver.processDatagram(packet.data(), packet.size());

            if (nuFrame % 25 == 0) {
                std::this_thread::yield();
            }
        }
        writerDone.store(true, std::memory_order_release);
    });

    // Reader thread simulating high-frequency JNI getTelemetry() polling
    std::thread reader([&]() {
        while (!writerDone.load(std::memory_order_acquire) || readSnapshots.load(std::memory_order_relaxed) < 200) {
            const auto telem = receiver.getTelemetry();
            vban::NativeTelemetry snapshot;
            snapshot.sampleRate = telem.activeSampleRate;
            snapshot.channelCount = telem.activeChannels;
            snapshot.bitDepth = telem.activeBitDepth;
            snapshot.totalPacketsReceived = telem.totalPacketsReceived;
            snapshot.lostPackets = telem.lostPackets;
            snapshot.jitterMs = telem.jitterMs;
            snapshot.bufferOccupancyPercent = ringBuffer.occupancyPercent();
            snapshot.peakLevelLeft = receiver.getPeakLeft();
            snapshot.peakLevelRight = receiver.getPeakRight();

            // Invariant assertions
            if (snapshot.sampleRate != 48000 || snapshot.channelCount != 2 || snapshot.bitDepth != 16) {
                consistencyViolation.store(true);
            }
            if (snapshot.lostPackets > snapshot.totalPacketsReceived) {
                consistencyViolation.store(true);
            }
            if (snapshot.bufferOccupancyPercent < 0.0f || snapshot.bufferOccupancyPercent > 100.0f) {
                consistencyViolation.store(true);
            }
            if (snapshot.peakLevelLeft < 0.0f || snapshot.peakLevelLeft > 1.0f) {
                consistencyViolation.store(true);
            }
            if (snapshot.peakLevelRight < 0.0f || snapshot.peakLevelRight > 1.0f) {
                consistencyViolation.store(true);
            }

            // Drain ring buffer incrementally to simulate audio consumer
            float drain[64];
            ringBuffer.read(drain, 64);

            readSnapshots.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    writer.join();
    reader.join();

    EXPECT_FALSE(consistencyViolation.load());
    ASSERT_GE(readSnapshots.load(), 200U);
}

TEST_CASE("receiver_buffer_saturation_channel_alignment_preservation") {
    // Verifies that under buffer saturation, whole-frame alignment is strictly preserved
    // and Left and Right audio channels NEVER swap or invert phase.
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    // Synthesize a stereo VBAN packet where:
    // Left channel samples are always POSITIVE (+0.5f)
    // Right channel samples are always NEGATIVE (-0.5f)
    const uint8_t nbs = 255; // 256 samples per frame
    const uint8_t nbc = 1;   // 2 channels (stereo)
    const size_t payloadBytes = vban::calculatePayloadBytes(nbs, nbc, sizeof(int16_t));

    std::vector<uint8_t> packetData(vban::VBAN_HEADER_SIZE + payloadBytes, 0);
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packetData.data());
    hdr->magic = vban::VBAN_MAGIC;
    hdr->format_SR = static_cast<uint8_t>(vban::VBanSubProtocol::Audio) | (3 & 0x1F); // 48 kHz
    hdr->format_nbs = nbs;
    hdr->format_nbc = nbc;
    hdr->format_bit = static_cast<uint8_t>(vban::VBanCodec::PCM) | (static_cast<uint8_t>(vban::VBanDataType::Int16) & 0x07);
    std::memcpy(hdr->streamName, "Stream1", 7);

    auto* samples = reinterpret_cast<int16_t*>(packetData.data() + vban::VBAN_HEADER_SIZE);
    for (size_t i = 0; i <= nbs; ++i) {
        samples[2 * i + 0] = 16384;   // Left: +0.5f
        samples[2 * i + 1] = -16384;  // Right: -0.5f
    }

    // 1. Deliberately prime the ring buffer so that exactly 3 free slots remain (an odd number)
    const size_t targetFreeSlots = 3;
    const size_t prefillCount = vban::VBAN_RING_BUFFER_CAPACITY - targetFreeSlots;
    std::vector<float> prefill(prefillCount, 0.0f);
    ASSERT_EQ(ringBuffer.write(prefill.data(), prefillCount), prefillCount);
    ASSERT_EQ(ringBuffer.availableWrite(), targetFreeSlots);

    // 2. Feed the stereo packet into the receiver
    // Available is 3. Max whole frames = 3 / 2 = 1 frame = 2 samples.
    // The receiver must write exactly 2 samples, leaving 1 slot free, and record 255 overflow frames.
    hdr->nuFrame = 1;
    ASSERT_TRUE(receiver.processDatagram(packetData.data(), packetData.size()));
    EXPECT_EQ(ringBuffer.availableWrite(), 1U);

    auto telem1 = receiver.getTelemetry();
    EXPECT_EQ(telem1.overflowFrames, 255U);

    // Drain the pre-filled dummy samples
    std::vector<float> drain(prefillCount);
    size_t drained = 0;
    while (drained < prefillCount) {
        drained += ringBuffer.read(drain.data() + drained, prefillCount - drained);
    }
    ASSERT_EQ(drained, prefillCount);

    // Read the 2 samples that were written from packet 1
    std::array<float, 2> pkt1Samples{};
    ASSERT_EQ(ringBuffer.read(pkt1Samples.data(), 2), 2U);
    ASSERT_GT(pkt1Samples[0], 0.49f);  // Left channel strictly positive
    ASSERT_LT(pkt1Samples[1], -0.49f); // Right channel strictly negative

    // 3. Now inundate the empty buffer with 600 packets to trigger massive buffer saturation
    for (uint32_t nu = 2; nu <= 600; ++nu) {
        hdr->nuFrame = nu;
        receiver.processDatagram(packetData.data(), packetData.size());
    }

    auto telem2 = receiver.getTelemetry();
    ASSERT_GT(telem2.overflowFrames, 0U);

    // 4. Drain all remaining frames and verify that channel alignment was never swapped!
    size_t totalSamplesRead = 0;
    size_t channelSwapViolations = 0;
    std::array<float, 512> readChunk{};

    while (ringBuffer.availableRead() >= 2) {
        size_t toRead = (ringBuffer.availableRead() / 2) * 2;
        toRead = std::min(toRead, readChunk.size());
        size_t actualRead = ringBuffer.read(readChunk.data(), toRead);
        for (size_t f = 0; f < actualRead / 2; ++f) {
            float l = readChunk[2 * f + 0];
            float r = readChunk[2 * f + 1];
            if (l <= 0.0f || r >= 0.0f) {
                channelSwapViolations++;
            }
        }
        totalSamplesRead += actualRead;
    }

    ASSERT_GT(totalSamplesRead, 0U);
    EXPECT_EQ(channelSwapViolations, 0U);
}

TEST_CASE("receiver_promiscuous_mode_accepts_arbitrary_stream_names") {
    WsaGuard wsa;
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);

    vban::ReceiverSocketConfig cfg;
    cfg.listenIp = "127.0.0.1";
    cfg.port = 17985;
    cfg.streamName = ""; // Promiscuous mode

    ASSERT_TRUE(receiver.start(cfg));

    auto packet1 = createTestVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Voicemeeter");
    auto* hdr1 = reinterpret_cast<vban::VBanHeader*>(packet1.data());
    hdr1->nuFrame = 1;
    EXPECT_TRUE(receiver.processDatagram(packet1.data(), packet1.size()));
    EXPECT_EQ(receiver.getDetectedStreamName(), "Voicemeeter");

    auto telem1 = receiver.getTelemetry();
    EXPECT_EQ(std::string(telem1.activeStreamName), "Voicemeeter");
    EXPECT_EQ(telem1.validPackets, 1U);

    auto packet2 = createTestVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "CustomStream2");
    auto* hdr2 = reinterpret_cast<vban::VBanHeader*>(packet2.data());
    hdr2->nuFrame = 2;
    EXPECT_TRUE(receiver.processDatagram(packet2.data(), packet2.size()));
    EXPECT_EQ(receiver.getDetectedStreamName(), "CustomStream2");

    auto telem2 = receiver.getTelemetry();
    EXPECT_EQ(std::string(telem2.activeStreamName), "CustomStream2");
    EXPECT_EQ(telem2.validPackets, 2U);

    receiver.stop();
}
