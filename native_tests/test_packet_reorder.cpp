#include "test_framework.hpp"
#include "packet_reorder_queue.hpp"
#include "vban_receiver.hpp"

#include <vector>
#include <array>
#include <cmath>
#include <cstring>

namespace {

std::vector<uint8_t> createReorderTestPacket(uint32_t nuFrame, float constantValue) {
    constexpr uint8_t nbs = 255; // 256 samples
    constexpr uint8_t nbc = 1;   // 2 channels
    constexpr size_t totalSamples = 256 * 2;
    constexpr size_t payloadBytes = totalSamples * sizeof(float);
    std::vector<uint8_t> packet(vban::VBAN_HEADER_SIZE + payloadBytes, 0);

    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    hdr->magic = vban::VBAN_MAGIC;
    hdr->format_SR = (static_cast<uint8_t>(vban::VBanSubProtocol::Audio)) | 3; // 48 kHz
    hdr->format_nbs = nbs;
    hdr->format_nbc = nbc;
    hdr->format_bit = (static_cast<uint8_t>(vban::VBanCodec::PCM)) | static_cast<uint8_t>(vban::VBanDataType::Float32);

    std::memset(hdr->streamName, 0, 16);
    std::memcpy(hdr->streamName, "Stream1", 7);
    hdr->nuFrame = nuFrame;

    auto* samples = reinterpret_cast<float*>(packet.data() + vban::VBAN_HEADER_SIZE);
    for (size_t i = 0; i < totalSamples; ++i) {
        samples[i] = constantValue;
    }

    return packet;
}

} // namespace

TEST_CASE("PacketReorderQueue reassembles out-of-order sequence (1, 3, 2, 4)") {
    vban::PacketReorderQueue<float, 16, 256 * 2> queue;
    float p1[4] = {1.f, 1.f, 1.f, 1.f};
    float p2[4] = {2.f, 2.f, 2.f, 2.f};
    float p3[4] = {3.f, 3.f, 3.f, 3.f};
    float p4[4] = {4.f, 4.f, 4.f, 4.f};

    ASSERT_TRUE(queue.push(101, p1, 4));
    ASSERT_TRUE(queue.push(103, p3, 4));
    ASSERT_TRUE(queue.push(102, p2, 4));
    ASSERT_TRUE(queue.push(104, p4, 4));

    float out[16] = {};
    bool plc = false;
    size_t popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 16U);
    ASSERT_FALSE(plc);
    ASSERT_EQ(out[0], 1.f);
    ASSERT_EQ(out[4], 2.f);
    ASSERT_EQ(out[8], 3.f);
    ASSERT_EQ(out[12], 4.f);
}

TEST_CASE("PacketReorderQueue handles incremental out-of-order arrival") {
    vban::PacketReorderQueue<float, 16, 512> queue;
    queue.setReorderDepth(2);
    queue.setTimeoutMs(50.0);

    float p1[4] = {1.f, 1.f, 1.f, 1.f};
    float p2[4] = {2.f, 2.f, 2.f, 2.f};
    float p3[4] = {3.f, 3.f, 3.f, 3.f};

    // 1. Push 101 -> immediately ready
    ASSERT_TRUE(queue.push(101, p1, 4));
    float out[16] = {};
    bool plc = false;
    size_t popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 4U);
    ASSERT_FALSE(plc);
    ASSERT_EQ(out[0], 1.f);

    // 2. Push 103 (102 missing, within reorder depth) -> popOrdered waits for 102
    ASSERT_TRUE(queue.push(103, p3, 4));
    popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 0U);

    // 3. Push 102 -> both 102 and 103 should now be popped in exact sequence
    ASSERT_TRUE(queue.push(102, p2, 4));
    popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 8U);
    ASSERT_FALSE(plc);
    ASSERT_EQ(out[0], 2.f);
    ASSERT_EQ(out[4], 3.f);
}

TEST_CASE("PacketReorderQueue single-packet loss PLC extrapolation with raised-cosine decay") {
    vban::PacketReorderQueue<float, 16, 512> queue;
    queue.setTimeoutMs(0.0); // Immediate expiration on missing slot check

    float p1[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float p3[4] = {2.0f, 2.0f, 2.0f, 2.0f};

    // 1. Push 101
    ASSERT_TRUE(queue.push(101, p1, 4));
    float out[16] = {};
    bool plc = false;
    size_t popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 4U);
    ASSERT_FALSE(plc);
    ASSERT_EQ(out[0], 1.0f);

    // 2. 102 never arrives, 103 arrives
    ASSERT_TRUE(queue.push(103, p3, 4));

    // popOrdered should detect 1-packet gap at 102, expire it, synthesize 1 packet via PLC,
    // and then deliver 103
    popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 8U);
    ASSERT_TRUE(plc);

    // First 4 samples correspond to extrapolated 102: smooth raised-cosine decay from 1.0f towards 0.0f
    ASSERT_NEAR(out[0], 1.0f, 1e-4f);
    ASSERT_LE(out[1], out[0]);
    ASSERT_LE(out[2], out[1]);
    ASSERT_LE(out[3], out[2]);
    ASSERT_GT(out[3], 0.0f);

    // Next 4 samples correspond to packet 103 (2.0f)
    ASSERT_EQ(out[4], 2.0f);
    ASSERT_EQ(out[5], 2.0f);
    ASSERT_EQ(out[6], 2.0f);
    ASSERT_EQ(out[7], 2.0f);
}

TEST_CASE("PacketReorderQueue gap > 1 flushes queue and resets sequence tracking") {
    vban::PacketReorderQueue<float, 16, 512> queue;

    float p1[4] = {1.f, 1.f, 1.f, 1.f};
    float p5[4] = {5.f, 5.f, 5.f, 5.f};

    ASSERT_TRUE(queue.push(101, p1, 4));
    float out[16] = {};
    bool plc = false;
    size_t popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 4U);
    ASSERT_EQ(out[0], 1.f);

    // Gap > 1 packet: 101 -> 105 (missing 102, 103, 104)
    ASSERT_TRUE(queue.push(105, p5, 4));

    popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 4U);
    ASSERT_FALSE(plc);
    ASSERT_EQ(out[0], 5.f);
    ASSERT_EQ(queue.getExpectedNuFrame(), 106U);
}

TEST_CASE("PacketReorderQueue rejects late and duplicate packets") {
    vban::PacketReorderQueue<float, 16, 512> queue;
    float p1[4] = {1.f, 1.f, 1.f, 1.f};

    ASSERT_TRUE(queue.push(101, p1, 4));

    // Duplicate while unpopped in queue -> rejected
    ASSERT_FALSE(queue.push(101, p1, 4));

    float out[8] = {};
    bool plc = false;
    queue.popOrdered(out, 8, plc);

    // Duplicate of already popped packet 101 -> rejected
    ASSERT_FALSE(queue.push(101, p1, 4));

    // Older packet 100 -> rejected
    ASSERT_FALSE(queue.push(100, p1, 4));
}

TEST_CASE("PacketReorderQueue handles sequence jump >= 3 without premature flush (101, 104, 102, 103)") {
    vban::PacketReorderQueue<float, 16, 256 * 2> queue;
    float p1[4] = {1.f, 1.f, 1.f, 1.f};
    float p2[4] = {2.f, 2.f, 2.f, 2.f};
    float p3[4] = {3.f, 3.f, 3.f, 3.f};
    float p4[4] = {4.f, 4.f, 4.f, 4.f};

    // Push 101, then jump to 104 (jump = 3), then out-of-order 102 and 103
    ASSERT_TRUE(queue.push(101, p1, 4));
    ASSERT_TRUE(queue.push(104, p4, 4));
    ASSERT_TRUE(queue.push(102, p2, 4));
    ASSERT_TRUE(queue.push(103, p3, 4));

    float out[16] = {};
    bool plc = false;
    size_t popped = queue.popOrdered(out, 16, plc);
    ASSERT_EQ(popped, 16U);
    ASSERT_FALSE(plc);
    ASSERT_EQ(out[0], 1.f);
    ASSERT_EQ(out[4], 2.f);
    ASSERT_EQ(out[8], 3.f);
    ASSERT_EQ(out[12], 4.f);
}

TEST_CASE("PacketReorderQueue modular wrap-around across 0xFFFFFFFF with sequence jump >= 3") {
    vban::PacketReorderQueue<float, 16, 512> queue;
    float pA[4] = {10.f, 10.f, 10.f, 10.f};
    float pB[4] = {20.f, 20.f, 20.f, 20.f};
    float pC[4] = {30.f, 30.f, 30.f, 30.f};
    float pD[4] = {40.f, 40.f, 40.f, 40.f};
    float pE[4] = {50.f, 50.f, 50.f, 50.f};

    // Expected order: 0xFFFFFFFD, 0xFFFFFFFE, 0xFFFFFFFF, 0x00000000, 0x00000001
    // Push with jump = 4 across 0xFFFFFFFF boundary
    ASSERT_TRUE(queue.push(0xFFFFFFFDU, pA, 4));
    ASSERT_TRUE(queue.push(0x00000001U, pE, 4)); // Jump of 4 across wrap
    ASSERT_TRUE(queue.push(0xFFFFFFFFU, pC, 4));
    ASSERT_TRUE(queue.push(0xFFFFFFFEU, pB, 4));
    ASSERT_TRUE(queue.push(0x00000000U, pD, 4));

    // Duplicate rejection for unpopped packet in circular slot across wrap
    ASSERT_FALSE(queue.push(0x00000000U, pD, 4));

    float out[20] = {};
    bool plc = false;
    size_t popped = queue.popOrdered(out, 20, plc);
    ASSERT_EQ(popped, 20U);
    ASSERT_FALSE(plc);
    ASSERT_EQ(out[0], 10.f);
    ASSERT_EQ(out[4], 20.f);
    ASSERT_EQ(out[8], 30.f);
    ASSERT_EQ(out[12], 40.f);
    ASSERT_EQ(out[16], 50.f);
}

TEST_CASE("VbanReceiver reorders out-of-order UDP datagrams before SPSC push") {
    vban::SpscRingBuffer<float, vban::VBAN_RING_BUFFER_CAPACITY> ringBuffer;
    vban::VbanReceiver receiver(ringBuffer);
    receiver.getReorderQueue().setReorderDepth(2);
    receiver.getReorderQueue().setTimeoutMs(50.0);

    auto pkt101 = createReorderTestPacket(101, 0.2f);
    auto pkt102 = createReorderTestPacket(102, 0.5f);
    auto pkt103 = createReorderTestPacket(103, 0.8f);

    // 1. Ingest 101 -> immediately pushed to ring buffer
    ASSERT_TRUE(receiver.processDatagram(pkt101.data(), pkt101.size()));
    ASSERT_EQ(ringBuffer.availableRead(), 512U);

    // 2. Ingest 103 (102 missing, within reorder depth) -> held in reorder queue
    ASSERT_TRUE(receiver.processDatagram(pkt103.data(), pkt103.size()));
    ASSERT_EQ(ringBuffer.availableRead(), 512U); // No new samples popped yet!

    // 3. Ingest 102 (out of order arrival) -> both 102 and 103 flushed in order to ring buffer
    ASSERT_TRUE(receiver.processDatagram(pkt102.data(), pkt102.size()));
    ASSERT_EQ(ringBuffer.availableRead(), 1536U); // 512 * 3 = 1536 samples

    // Verify ordering in ring buffer
    std::vector<float> readAudio(1536, 0.0f);
    size_t readCount = ringBuffer.read(readAudio.data(), 1536);
    ASSERT_EQ(readCount, 1536U);

    // Check packet 101 samples (0..511) are 0.2f
    for (size_t i = 0; i < 512; ++i) {
        ASSERT_NEAR(readAudio[i], 0.2f, 1e-4f);
    }
    // Check packet 102 samples (512..1023) are 0.5f
    for (size_t i = 512; i < 1024; ++i) {
        ASSERT_NEAR(readAudio[i], 0.5f, 1e-4f);
    }
    // Check packet 103 samples (1024..1535) are 0.8f
    for (size_t i = 1024; i < 1536; ++i) {
        ASSERT_NEAR(readAudio[i], 0.8f, 1e-4f);
    }
}
