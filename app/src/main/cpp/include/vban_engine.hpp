#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <span>
#include <concepts>
#include <bit>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <oboe/Oboe.h>

#include "vban_protocol.hpp"
#include "vban_parser.hpp"
#include "vban_converter.hpp"
#include "spsc_ring_buffer.hpp"
#include "drift_compensator.hpp"
#include "oboe_player.hpp"
#include "vban_receiver.hpp"

namespace vban {

/**
 * Audio stream configuration matching PROJECT.md interface contract.
 */
struct AudioStreamConfig {
    uint32_t sampleRate{48000};
    uint16_t channelCount{2};
    int32_t  targetDeviceId{0};
};

/**
 * VbanEngine
 * 
 * Core coordinator managing the lifecycle of the VBAN audio reception,
 * packet processing, jitter buffer, and Oboe low-latency playback pipeline.
 */
class VbanEngine {
public:
    static VbanEngine& getInstance();

    VbanEngine();
    ~VbanEngine();

    // Prevent copy and move semantics (singleton audio engine)
    VbanEngine(const VbanEngine&) = delete;
    VbanEngine& operator=(const VbanEngine&) = delete;
    VbanEngine(VbanEngine&&) = delete;
    VbanEngine& operator=(VbanEngine&&) = delete;

    /**
     * Initialize the native engine.
     * @return true if initialization succeeded.
     */
    bool initialize();

    /**
     * Check if engine is initialized.
     */
    bool isInitialized() const noexcept;

    /**
     * Start the VBAN reception and audio playback pipeline.
     * @param listenIp Local or multicast IP to bind (e.g. "0.0.0.0" or "239.255.0.1")
     * @param port UDP port (typically 6980)
     * @param streamName Null-terminated stream name filter (max 16 chars)
     * @param deviceId Target audio output device ID (0 = default)
     * @return true if started successfully.
     */
    bool start(const std::string& listenIp, uint16_t port, const std::string& streamName, int32_t deviceId = 0);

    /**
     * Stop the pipeline and release audio/network resources.
     */
    void stop();

    /**
     * Check if engine is actively running.
     */
    bool isRunning() const noexcept;

    /**
     * Route audio to a specific output device (e.g. USB DAC).
     * @param deviceId Target audio device ID.
     */
    void setAudioDevice(int32_t deviceId);

    /**
     * Reconfigure audio stream format (sample rate, channels).
     * Executes fade-out, rebuilds Oboe stream, executes fade-in.
     * @param config New stream configuration.
     * @return true if reconfiguration succeeded.
     */
    bool reconfigureStream(const AudioStreamConfig& config);

    /**
     * Poll latest telemetry snapshot (non-blocking, lock-free).
     * @return Current telemetry snapshot.
     */
    NativeTelemetry getTelemetry() const noexcept;

    /**
     * Configures target audio latency / buffer watermark in milliseconds.
     */
    bool setTargetLatencyMs(float latencyMs);

    /**
     * Polls latest real-time FFT spectrum bands for 60 FPS UI visualizer.
     * @param outBands Destination array receiving normalized magnitudes [0.0f, 1.0f].
     * @param numBands Band count (typically 32 or 64).
     * @return true if fresh FFT snapshot was returned.
     */
    bool getLatestFftBands(float* outBands, size_t numBands) noexcept;

    /**
     * Processes an incoming raw VBAN network datagram:
     * Validates header, dequantizes payload to Float32, updates peak telemetry,
     * and enqueues frames into the lock-free SPSC circular ring buffer.
     * 
     * @param buffer Raw datagram bytes
     * @param size Total datagram byte count
     * @return true if packet was valid and successfully enqueued
     */
    bool processPacket(const uint8_t* buffer, size_t size);

    /**
     * Access underlying VBAN protocol parser.
     */
    [[nodiscard]] VbanParser& getParser() noexcept { return parser_; }
    [[nodiscard]] const VbanParser& getParser() const noexcept { return parser_; }

    /**
     * Access underlying lock-free SPSC audio circular ring buffer.
     */
    [[nodiscard]] SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& getRingBuffer() noexcept { return ringBuffer_; }
    [[nodiscard]] const SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& getRingBuffer() const noexcept { return ringBuffer_; }

    /**
     * Access underlying clock drift compensator.
     */
    [[nodiscard]] DriftCompensator& getDriftCompensator() noexcept { return compensator_; }
    [[nodiscard]] const DriftCompensator& getDriftCompensator() const noexcept { return compensator_; }

    /**
     * Access underlying Oboe audio player.
     */
    [[nodiscard]] OboePlayer* getPlayer() noexcept { return player_.get(); }
    [[nodiscard]] const OboePlayer* getPlayer() const noexcept { return player_.get(); }

    /**
     * Access underlying VBAN UDP network receiver.
     */
    [[nodiscard]] VbanReceiver* getReceiver() noexcept { return receiver_.get(); }
    [[nodiscard]] const VbanReceiver* getReceiver() const noexcept { return receiver_.get(); }

private:
    void reconfigWorkerLoop();
    void onStreamFormatChanged(uint32_t newRate, uint16_t newChannels);

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<int32_t> currentDeviceId_{0};

    std::atomic<uint32_t> m_currentSampleRate{48000};
    std::atomic<uint16_t> m_currentChannels{2};
    std::string listenIp_{"0.0.0.0"};
    uint16_t listenPort_{6980};
    std::string targetStreamName_{"Stream1"};

    VbanParser parser_{"Stream1"};
    SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY> ringBuffer_;
    DriftCompensator compensator_;
    std::unique_ptr<OboePlayer> player_;
    std::unique_ptr<VbanReceiver> receiver_;

    // Asynchronous dynamic reconfiguration worker
    std::thread m_reconfigThread;
    std::mutex m_reconfigMutex;
    std::condition_variable m_reconfigCv;
    std::atomic<bool> m_stopReconfigWorker{false};
    std::atomic<bool> m_reconfigPending{false};
    std::atomic<uint32_t> m_pendingSampleRate{0};
    std::atomic<uint16_t> m_pendingChannels{0};

    mutable NativeTelemetry telemetryCache_{};
};

} // namespace vban
