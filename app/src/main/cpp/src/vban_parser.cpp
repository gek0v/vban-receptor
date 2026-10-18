#include "vban_parser.hpp"
#include <cmath>
#include <algorithm>

namespace vban {

VbanParser::VbanParser(std::string_view targetStreamName)
    : targetStreamName_(targetStreamName) {}

VbanParser::VbanParser(VbanParser&& other) noexcept
    : targetStreamName_(std::move(other.targetStreamName_)),
      totalPackets_(other.totalPackets_.load(std::memory_order_relaxed)),
      validPackets_(other.validPackets_.load(std::memory_order_relaxed)),
      lostPackets_(other.lostPackets_.load(std::memory_order_relaxed)),
      outOfOrderPackets_(other.outOfOrderPackets_.load(std::memory_order_relaxed)),
      hasReceivedFirst_(other.hasReceivedFirst_),
      lastNuFrame_(other.lastNuFrame_.load(std::memory_order_relaxed)),
      lastArrivalNs_(other.lastArrivalNs_),
      jitterSeconds_(other.jitterSeconds_.load(std::memory_order_relaxed)) {}

VbanParser& VbanParser::operator=(VbanParser&& other) noexcept {
    if (this != &other) {
        targetStreamName_ = std::move(other.targetStreamName_);
        totalPackets_.store(other.totalPackets_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        validPackets_.store(other.validPackets_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        lostPackets_.store(other.lostPackets_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        outOfOrderPackets_.store(other.outOfOrderPackets_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        hasReceivedFirst_ = other.hasReceivedFirst_;
        lastNuFrame_.store(other.lastNuFrame_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        lastArrivalNs_ = other.lastArrivalNs_;
        jitterSeconds_.store(other.jitterSeconds_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
}

void VbanParser::setTargetStreamName(std::string_view targetStreamName) {
    targetStreamName_ = std::string(targetStreamName);
}

std::string_view VbanParser::getTargetStreamName() const noexcept {
    return targetStreamName_;
}

void VbanParser::reset() noexcept {
    totalPackets_.store(0, std::memory_order_relaxed);
    validPackets_.store(0, std::memory_order_relaxed);
    lostPackets_.store(0, std::memory_order_relaxed);
    outOfOrderPackets_.store(0, std::memory_order_relaxed);
    hasReceivedFirst_ = false;
    lastNuFrame_.store(0, std::memory_order_relaxed);
    lastArrivalNs_ = 0;
    jitterSeconds_.store(0.0, std::memory_order_relaxed);
}

ParserTelemetry VbanParser::getTelemetry() const noexcept {
    const uint64_t valid = validPackets_.load(std::memory_order_acquire);
    const uint64_t lost = lostPackets_.load(std::memory_order_relaxed);
    const uint64_t ooo = outOfOrderPackets_.load(std::memory_order_relaxed);
    uint64_t total = totalPackets_.load(std::memory_order_acquire);
    if (total < valid) {
        total = valid;
    }
    const double jitter = jitterSeconds_.load(std::memory_order_relaxed);
    const uint32_t lastSeq = lastNuFrame_.load(std::memory_order_relaxed);

    return ParserTelemetry{
        .totalPacketsReceived = total,
        .validPackets = valid,
        .lostPackets = lost,
        .outOfOrderPackets = ooo,
        .jitterMs = jitter * 1000.0,
        .lastSequenceNumber = lastSeq
    };
}

const char* VbanParser::resultToString(ParseResult result) noexcept {
    switch (result) {
        case ParseResult::Success:                return "Success";
        case ParseResult::PacketTooSmall:          return "PacketTooSmall";
        case ParseResult::InvalidMagic:            return "InvalidMagic";
        case ParseResult::UnsupportedSubprotocol:  return "UnsupportedSubprotocol";
        case ParseResult::UnsupportedCodec:        return "UnsupportedCodec";
        case ParseResult::ReservedBitSet:          return "ReservedBitSet";
        case ParseResult::InvalidSampleRate:       return "InvalidSampleRate";
        case ParseResult::UnsupportedDataType:     return "UnsupportedDataType";
        case ParseResult::StreamNameMismatch:      return "StreamNameMismatch";
        case ParseResult::PayloadSizeMismatch:     return "PayloadSizeMismatch";
    }
    return "UnknownError";
}

bool VbanParser::matchStreamName(std::string_view candidate) const noexcept {
    if (targetStreamName_.empty()) {
        return true; // Wildcard / promiscuous mode
    }
    if (candidate.size() != targetStreamName_.size()) {
        return false;
    }
    // Case-insensitive ASCII comparison
    return std::equal(candidate.begin(), candidate.end(),
                      targetStreamName_.begin(), targetStreamName_.end(),
                      [](char a, char b) noexcept {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

void VbanParser::updateSequenceAndJitter(
    uint32_t nuFrame,
    uint32_t sampleRate,
    uint16_t samplesPerFrame,
    uint64_t arrivalTimeNs) noexcept {

    if (!hasReceivedFirst_) {
        hasReceivedFirst_ = true;
        lastNuFrame_.store(nuFrame, std::memory_order_relaxed);
        lastArrivalNs_ = arrivalTimeNs;
        jitterSeconds_.store(0.0, std::memory_order_relaxed);
        return;
    }

    // 32-bit unsigned modular sequence difference
    const uint32_t lastNu = lastNuFrame_.load(std::memory_order_relaxed);
    const uint32_t delta = nuFrame - lastNu;

    if (delta == 1) {
        // Sequential packet: 0 loss
    } else if (delta > 1 && delta < 0x80000000U) {
        // Forward gap: packet loss detected
        lostPackets_.fetch_add(delta - 1, std::memory_order_relaxed);
    } else {
        // Out-of-order or duplicate packet (delta == 0 or delta >= 0x80000000U)
        outOfOrderPackets_.fetch_add(1, std::memory_order_relaxed);
        // Do not advance lastNuFrame_ or update jitter for out-of-order packets
        return;
    }

    // RFC 3550 Inter-Arrival Jitter Estimation
    // D(i, j) = | (R_j - R_i) - (S_j - S_i) |
    // J(i) = J(i-1) + (|D| - J(i-1)) / 16.0
    if (sampleRate > 0 && arrivalTimeNs >= lastArrivalNs_) {
        const double elapsedArrivalSec = static_cast<double>(arrivalTimeNs - lastArrivalNs_) * 1e-9;
        const double nominalPacketSec  = static_cast<double>(samplesPerFrame) / static_cast<double>(sampleRate);
        const double nominalIntervalSec = static_cast<double>(delta) * nominalPacketSec;

        const double d = std::abs(elapsedArrivalSec - nominalIntervalSec);
        const double currentJitter = jitterSeconds_.load(std::memory_order_relaxed);
        const double newJitter = currentJitter + (d - currentJitter) / 16.0;
        jitterSeconds_.store(newJitter, std::memory_order_relaxed);
    }

    lastNuFrame_.store(nuFrame, std::memory_order_relaxed);
    lastArrivalNs_ = arrivalTimeNs;
}

ParseResult VbanParser::parse(
    const uint8_t* buffer,
    size_t size,
    uint64_t arrivalTimeNs,
    ParsedPacket& outPacket) noexcept {

    totalPackets_.fetch_add(1, std::memory_order_release);

    // Step 1: Minimum packet size (at least 28-byte header)
    if (buffer == nullptr || size < VBAN_HEADER_SIZE) {
        return ParseResult::PacketTooSmall;
    }

    const auto* header = reinterpret_cast<const VBanHeader*>(buffer);

    // Step 2: Magic FourCC check ('V','B','A','N' == 0x4E414256 LE)
    if (header->magic != VBAN_MAGIC) {
        return ParseResult::InvalidMagic;
    }

    // Step 3: Audio subprotocol check (0x00)
    if (getSubProtocol(header->format_SR) != VBanSubProtocol::Audio) {
        return ParseResult::UnsupportedSubprotocol;
    }

    // Step 4: Codec check (0x00 PCM)
    if (getCodec(header->format_bit) != VBanCodec::PCM) {
        return ParseResult::UnsupportedCodec;
    }

    // Step 5: Reserved Bit 3 check (must be 0)
    if (isReservedBitSet(header->format_bit)) {
        return ParseResult::ReservedBitSet;
    }

    // Step 6: Sample rate lookup (indices 0..21)
    const auto srOpt = getSampleRate(header->format_SR);
    if (!srOpt.has_value()) {
        return ParseResult::InvalidSampleRate;
    }
    const uint32_t sampleRate = *srOpt;

    // Step 7: Data type support check (Int16, Int24, Int32, Float32)
    const VBanDataType dataType = getDataType(header->format_bit);
    if (dataType != VBanDataType::Int16 &&
        dataType != VBanDataType::Int24 &&
        dataType != VBanDataType::Int32 &&
        dataType != VBanDataType::Float32) {
        return ParseResult::UnsupportedDataType;
    }

    // Step 8: Stream name matching
    const std::string_view streamName = extractStreamName(header->streamName);
    if (!matchStreamName(streamName)) {
        return ParseResult::StreamNameMismatch;
    }

    // Step 9: Exact payload size validation
    const uint8_t sampleSize = getSampleSize(dataType);
    const size_t expectedPayload = calculatePayloadBytes(header->format_nbs, header->format_nbc, sampleSize);
    const size_t expectedTotalBytes = VBAN_HEADER_SIZE + expectedPayload;

    if (size != expectedTotalBytes) {
        return ParseResult::PayloadSizeMismatch;
    }

    // Populate output metadata
    const uint16_t samplesPerFrame = static_cast<uint16_t>(header->format_nbs) + 1;
    const uint16_t channels        = static_cast<uint16_t>(header->format_nbc) + 1;

    outPacket.header          = header;
    outPacket.payload         = buffer + VBAN_HEADER_SIZE;
    outPacket.payloadBytes    = expectedPayload;
    outPacket.sampleRate      = sampleRate;
    outPacket.channels        = channels;
    outPacket.samplesPerFrame = samplesPerFrame;
    outPacket.sampleSize      = sampleSize;
    outPacket.dataType        = dataType;
    outPacket.streamName      = streamName;
    outPacket.frameNumber     = header->nuFrame;

    // Step 10: Monotonic frame loss tracking and RFC 3550 jitter calculation
    updateSequenceAndJitter(header->nuFrame, sampleRate, samplesPerFrame, arrivalTimeNs);
    validPackets_.fetch_add(1, std::memory_order_release);

    return ParseResult::Success;
}

ParseResult VbanParser::parse(
    const uint8_t* buffer,
    size_t size,
    ParsedPacket& outPacket) noexcept {

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return parse(buffer, size, nowNs, outPacket);
}

} // namespace vban
