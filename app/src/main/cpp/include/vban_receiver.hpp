#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <span>

#include "vban_protocol.hpp"
#include "vban_parser.hpp"
#include "vban_converter.hpp"
#include "spsc_ring_buffer.hpp"
#include "packet_reorder_queue.hpp"

namespace vban {

/**
 * Socket configuration options for VbanReceiver.
 */
struct ReceiverSocketConfig {
    std::string listenIp{"0.0.0.0"};
    uint16_t    port{6980};
    std::string streamName{""};
    size_t      socketBufferSize{4 * 1024 * 1024}; // 4 MB SO_RCVBUF
    uint32_t    receiveTimeoutMs{100};              // 100 ms SO_RCVTIMEO
};

using ReceiverConfig = ReceiverSocketConfig;

/**
 * Thread-safe lock-free telemetry snapshot matching PROJECT.md interface contract:
 * Exactly 40 bytes, matching JNI constructor descriptor (IIIJJFFFF)V.
 */
struct NativeTelemetry {
    uint32_t sampleRate{0};
    uint16_t channelCount{0};
    uint16_t bitDepth{0};
    uint64_t totalPacketsReceived{0};
    uint64_t lostPackets{0};
    float    jitterMs{0.0f};
    float    bufferOccupancyPercent{0.0f};
    float    peakLevelLeft{0.0f};
    float    peakLevelRight{0.0f};
    float    targetWatermarkMs{0.0f};
    uint64_t unrecoverableDrops{0};
    uint64_t glitchCount{0};
};

/**
 * Receiver telemetry counters.
 */
struct ReceiverTelemetry {
    uint64_t totalBytesReceived{0};
    uint64_t totalPacketsReceived{0};
    uint64_t validPackets{0};
    uint64_t lostPackets{0};
    uint64_t outOfOrderPackets{0};
    uint64_t overflowFrames{0};
    float    jitterMs{0.0f};
    uint32_t activeSampleRate{0};
    uint16_t activeChannels{0};
    uint16_t activeBitDepth{0};
    uint32_t grantedBufferSize{0};
    bool     isMulticast{false};
    bool     isRunning{false};
    char     activeStreamName[17]{0};
};

/**
 * Callback signature for dynamic stream reconfiguration notifications.
 * Parameter 1: new sample rate (Hz)
 * Parameter 2: new channel count (1..8)
 */
using ReconfigureCallback = std::function<void(uint32_t sampleRate, uint16_t channels)>;

/**
 * VbanReceiver
 * 
 * High-performance, zero-allocation network receiver engine for VBAN audio packets:
 * - Cross-platform BSD POSIX / Winsock abstraction.
 * - 4 MB SO_RCVBUF with descending ladder fallback.
 * - SO_REUSEADDR and SO_REUSEPORT (where available).
 * - IPv4 Class D multicast group membership (IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP).
 * - SO_RCVTIMEO (100 ms) + socket shutdown for instant cancellation.
 * - High-priority worker thread receive loop.
 * - 16 KB stack receive buffer, zero heap allocations in inner loop.
 * - Monotonic sequence wrap tracking, RFC 3550 EWMA jitter estimation via VbanParser.
 * - Format dequantization via VbanConverter.
 * - Decoupled dynamic stream reconfiguration trigger.
 * - Lock-free push into SpscRingBuffer<float, 262144>.
 */
class VbanReceiver {
public:
    static constexpr size_t RX_BUFFER_CAPACITY = 16384;   // 16 KB pre-allocated stack/member
    static constexpr size_t STAGING_MAX_SAMPLES = 4096;   // 4096 Float32 samples (16 ch * 256 frames)

    explicit VbanReceiver(
        SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& ringBuffer,
        ReconfigureCallback reconfigCb = nullptr
    );
    ~VbanReceiver();

    // Non-copyable and non-movable
    VbanReceiver(const VbanReceiver&) = delete;
    VbanReceiver& operator=(const VbanReceiver&) = delete;
    VbanReceiver(VbanReceiver&&) = delete;
    VbanReceiver& operator=(VbanReceiver&&) = delete;

    /**
     * Start the UDP network receiver and launch the worker thread.
     */
    bool start(const ReceiverSocketConfig& config);

    /**
     * Stop the worker thread, drop multicast membership, and close the network socket.
     */
    void stop();

    /**
     * Check if the network worker thread is actively running.
     */
    [[nodiscard]] bool isRunning() const noexcept;

    /**
     * Retrieve live telemetry counters snapshot (lock-free).
     */
    [[nodiscard]] ReceiverTelemetry getTelemetry() const noexcept;

    /**
     * Access underlying VBAN protocol parser.
     */
    [[nodiscard]] VbanParser& getParser() noexcept { return m_parser; }
    [[nodiscard]] const VbanParser& getParser() const noexcept { return m_parser; }

    /**
     * Register or update the reconfiguration callback.
     */
    void setReconfigureCallback(ReconfigureCallback cb);

    /**
     * Peak amplitude levels (linear [0.0f, 1.0f]).
     */
    [[nodiscard]] float getPeakLeft() const noexcept { return m_peakLeft.load(std::memory_order_relaxed); }
    [[nodiscard]] float getPeakRight() const noexcept { return m_peakRight.load(std::memory_order_relaxed); }

    /**
     * Granted socket buffer size in bytes.
     */
    [[nodiscard]] uint32_t getGrantedBufferSize() const noexcept { return m_grantedBufferSize; }

    /**
     * Get detected active stream name (thread-safe).
     */
    [[nodiscard]] std::string getDetectedStreamName() const {
        std::lock_guard<std::mutex> lock(m_streamNameMutex);
        return m_detectedStreamName;
    }

    /**
     * Process an incoming raw datagram (used by receive loop and for host testing / loopback injection).
     */
    bool processDatagram(const uint8_t* buffer, size_t size, uint64_t arrivalTimeNs) noexcept;

    /**
     * Overload measuring arrival time via steady_clock.
     */
    bool processDatagram(const uint8_t* buffer, size_t size) noexcept;

    /**
     * Alias for packet processing pipeline.
     */
    bool processPacket(const uint8_t* buffer, size_t size) noexcept {
        return processDatagram(buffer, size);
    }

    /**
     * Monotonic count of packets dropped or unrecoverable gaps from reorder queue.
     */
    [[nodiscard]] uint64_t getUnrecoverableDrops() const noexcept {
        return m_reorderQueue.getUnrecoverableDrops();
    }

    /**
     * Access underlying packet reorder and PLC queue.
     */
    [[nodiscard]] PacketReorderQueue<float, 16, STAGING_MAX_SAMPLES>& getReorderQueue() noexcept {
        return m_reorderQueue;
    }
    [[nodiscard]] const PacketReorderQueue<float, 16, STAGING_MAX_SAMPLES>& getReorderQueue() const noexcept {
        return m_reorderQueue;
    }

    /**
     * Helper: Detect if an IPv4 string belongs to Class D multicast (224.0.0.0 - 239.255.255.255).
     */
    [[nodiscard]] static bool isMulticastAddress(std::string_view ip) noexcept;
    [[nodiscard]] static bool isMulticastAddress(const char* ip) noexcept {
        if (!ip) return false;
        return isMulticastAddress(std::string_view(ip));
    }

private:
    void receiveLoop();
    bool openSocket(const ReceiverSocketConfig& config);
    void closeSocket();

    SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& m_ringBuffer;
    ReconfigureCallback m_reconfigCb;

    VbanParser m_parser;
    ReceiverSocketConfig m_config;

    // Network worker thread state
    std::thread m_workerThread;
    std::atomic<bool> m_running{false};

    // Platform-neutral socket descriptor (-1 = invalid)
    std::atomic<intptr_t> m_socketFd{-1};

    // Configuration and state
    uint32_t m_grantedBufferSize{0};
    bool     m_isMulticastJoined{false};
    alignas(4) uint8_t m_joinedMreq[8]{};

    mutable std::mutex m_streamNameMutex;
    std::string m_detectedStreamName;
    std::string m_workerStreamName;

    // Format tracking for dynamic reconfiguration
    std::atomic<uint32_t> m_activeSampleRate{48000};
    std::atomic<uint16_t> m_activeChannels{2};
    std::atomic<uint16_t> m_activeBitDepth{16};

    // Telemetry atomics
    std::atomic<uint64_t> m_totalBytesReceived{0};
    std::atomic<uint64_t> m_overflowFrames{0};
    std::atomic<float>    m_peakLeft{0.0f};
    std::atomic<float>    m_peakRight{0.0f};

    // UDP Packet Reorder & PLC Queue (16 slots)
    PacketReorderQueue<float, 16, STAGING_MAX_SAMPLES> m_reorderQueue;

    // Pre-allocated buffers (strictly zero runtime heap allocations in inner loop)
    alignas(16) std::array<uint8_t, RX_BUFFER_CAPACITY> m_rxBuffer{};
    alignas(64) std::array<float, STAGING_MAX_SAMPLES>  m_stagingBuffer{};
    alignas(64) std::array<float, STAGING_MAX_SAMPLES>  m_reorderPopBuffer{};
};

} // namespace vban
