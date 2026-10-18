#pragma once

#include "vban_protocol.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <atomic>
#include <chrono>
#include <ostream>

namespace vban {

/**
 * Validation return codes covering the 10-step packet pipeline.
 */
enum class ParseResult {
    Success = 0,
    PacketTooSmall,           // Size < 28 bytes
    InvalidMagic,             // Magic != 0x4E414256 ('VBAN')
    UnsupportedSubprotocol,   // Subprotocol != Audio (0x00)
    UnsupportedCodec,         // Codec != PCM (0x00)
    ReservedBitSet,           // Bit 3 of format_bit is non-zero
    InvalidSampleRate,        // Sample rate index >= 22
    UnsupportedDataType,      // Data type is not Int16, Int24, Int32, or Float32
    StreamNameMismatch,       // Stream name does not match target filter
    PayloadSizeMismatch       // Received size != 28 + (nbs+1)*(nbc+1)*sampleSize
};

/**
 * Decoded and verified packet metadata referencing payload bytes directly.
 */
struct ParsedPacket {
    const VBanHeader* header{nullptr};
    const uint8_t*    payload{nullptr};
    size_t            payloadBytes{0};
    uint32_t          sampleRate{0};
    uint16_t          channels{0};         // nbc + 1 (1..256)
    uint16_t          samplesPerFrame{0};  // nbs + 1 (1..256)
    uint8_t           sampleSize{0};       // Bytes per sample
    VBanDataType      dataType{VBanDataType::Int16};
    std::string_view  streamName{};
    uint32_t          frameNumber{0};      // nuFrame
};

/**
 * Thread-safe sequence and network telemetry snapshot.
 */
struct ParserTelemetry {
    uint64_t totalPacketsReceived{0};
    uint64_t validPackets{0};
    uint64_t lostPackets{0};
    uint64_t outOfOrderPackets{0};
    double   jitterMs{0.0};
    uint32_t lastSequenceNumber{0};
};

/**
 * VbanParser
 * 
 * Implements the strict 10-step VBAN packet validation pipeline,
 * frame sequence tracking, unsigned 32-bit wrap handling, and RFC 3550
 * inter-arrival jitter estimation.
 * 
 * Designed for zero heap allocations during real-time streaming.
 */
class VbanParser {
public:
    explicit VbanParser(std::string_view targetStreamName = "");
    ~VbanParser() = default;

    // Non-copyable, movable
    VbanParser(const VbanParser&) = delete;
    VbanParser& operator=(const VbanParser&) = delete;
    VbanParser(VbanParser&& other) noexcept;
    VbanParser& operator=(VbanParser&& other) noexcept;

    /**
     * Set target stream name filter. Empty string disables filtering (accept all).
     */
    void setTargetStreamName(std::string_view targetStreamName);

    /**
     * Get configured target stream name.
     */
    [[nodiscard]] std::string_view getTargetStreamName() const noexcept;

    /**
     * Reset sequence tracking, loss counters, and jitter state.
     */
    void reset() noexcept;

    /**
     * Executes the strict 10-step validation pipeline against an incoming datagram.
     * 
     * @param buffer Raw datagram bytes.
     * @param size Total datagram byte count.
     * @param arrivalTimeNs Monotonic reception timestamp in nanoseconds.
     * @param outPacket Destination struct populated upon ParseResult::Success.
     * @return ParseResult status code.
     */
    [[nodiscard]] ParseResult parse(
        const uint8_t* buffer,
        size_t size,
        uint64_t arrivalTimeNs,
        ParsedPacket& outPacket) noexcept;

    /**
     * Convenience overload measuring arrival time via std::chrono::steady_clock.
     */
    [[nodiscard]] ParseResult parse(
        const uint8_t* buffer,
        size_t size,
        ParsedPacket& outPacket) noexcept;

    // Telemetry Accessors
    [[nodiscard]] uint64_t getTotalPackets() const noexcept { return totalPackets_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t getValidPackets() const noexcept { return validPackets_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t getLostPackets() const noexcept { return lostPackets_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t getOutOfOrderPackets() const noexcept { return outOfOrderPackets_.load(std::memory_order_relaxed); }
    [[nodiscard]] double getJitterMs() const noexcept { return jitterSeconds_.load(std::memory_order_relaxed) * 1000.0; }
    [[nodiscard]] double getJitterSeconds() const noexcept { return jitterSeconds_.load(std::memory_order_relaxed); }
    [[nodiscard]] ParserTelemetry getTelemetry() const noexcept;

    /**
     * Helper stringifier for status codes.
     */
    [[nodiscard]] static const char* resultToString(ParseResult result) noexcept;

private:
    [[nodiscard]] bool matchStreamName(std::string_view candidate) const noexcept;
    void updateSequenceAndJitter(uint32_t nuFrame, uint32_t sampleRate, uint16_t samplesPerFrame, uint64_t arrivalTimeNs) noexcept;

    std::string targetStreamName_;

    // Telemetry & Sequence Tracking (Thread-safe atomics)
    std::atomic<uint64_t> totalPackets_{0};
    std::atomic<uint64_t> validPackets_{0};
    std::atomic<uint64_t> lostPackets_{0};
    std::atomic<uint64_t> outOfOrderPackets_{0};

    bool     hasReceivedFirst_{false};
    std::atomic<uint32_t> lastNuFrame_{0};
    uint64_t lastArrivalNs_{0};
    std::atomic<double>   jitterSeconds_{0.0};
};

inline std::ostream& operator<<(std::ostream& os, ParseResult res) {
    return os << VbanParser::resultToString(res);
}

} // namespace vban
