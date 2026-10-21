#pragma once

#include <oboe/Oboe.h>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>

#include "spsc_ring_buffer.hpp"
#include "drift_compensator.hpp"
#include "fft_analyzer.hpp"

namespace vban {

/**
 * Configuration parameters for Oboe Audio Player.
 */
struct OboePlayerConfig {
    uint32_t sampleRate{48000};
    uint16_t channelCount{2};
    int32_t  deviceId{0};
    oboe::AudioApi audioApi{oboe::AudioApi::AAudio};
    oboe::SharingMode sharingMode{oboe::SharingMode::Exclusive};
    oboe::PerformanceMode performanceMode{oboe::PerformanceMode::LowLatency};
    oboe::AudioFormat format{oboe::AudioFormat::Float};
};

/**
 * Production-ready Oboe 1.9.0 AAudio Low-Latency Player.
 *
 * Implements:
 * - Exclusive mode AAudio streaming up to 768 kHz (with fallback to Shared mode)
 * - Zero-allocation, lock-free real-time onAudioReady callback
 * - Signal processing pipeline through DriftCompensator
 * - Stereo peak telemetry extraction
 * - Asynchronous error recovery thread avoiding onErrorAfterClose deadlock
 * - Decoupled stream format reconfiguration with 128-sample cosine fade
 */
class OboePlayer : public oboe::AudioStreamDataCallback,
                   public oboe::AudioStreamErrorCallback {
public:
    explicit OboePlayer(SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& ringBuffer,
                        DriftCompensator& compensator);
    ~OboePlayer() override;

    // Non-copyable and non-movable
    OboePlayer(const OboePlayer&) = delete;
    OboePlayer& operator=(const OboePlayer&) = delete;
    OboePlayer(OboePlayer&&) = delete;
    OboePlayer& operator=(OboePlayer&&) = delete;

    /**
     * Start audio playback with specified configuration.
     * @param config Audio player configuration.
     * @return true if stream opened and started successfully.
     */
    bool start(const OboePlayerConfig& config);

    /**
     * Stop audio playback and close stream.
     */
    void stop();

    /**
     * Check if stream is currently active.
     */
    [[nodiscard]] bool isPlaying() const noexcept;

    /**
     * Reconfigure audio stream (sample rate, channel count, device ID).
     * Triggers 128-sample cosine fade-out, closes old stream, re-opens, and fades in.
     * @param sampleRate New sample rate in Hz.
     * @param channels New channel count (1 to 8).
     * @param deviceId Target audio output device ID (0 = default).
     * @return true if reconfiguration succeeded.
     */
    bool reconfigure(uint32_t sampleRate, uint16_t channels, int32_t deviceId);

    /**
     * Direct audio output device selection (e.g. USB DAC).
     * @param deviceId Target audio device ID.
     */
    void setAudioDevice(int32_t deviceId);

    // =========================================================================
    // Oboe Audio Stream Callbacks
    // =========================================================================

    /**
     * Real-time audio rendering callback (SCHED_FIFO high-priority thread).
     * Strictly ZERO memory allocations, ZERO mutex locks, ZERO syscalls.
     */
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                          void* audioData,
                                          int32_t numFrames) override;

    /**
     * Synchronous error callback called before stream closes.
     */
    void onErrorBeforeClose(oboe::AudioStream* stream, oboe::Result error) override;

    /**
     * Asynchronous error callback called after stream closes.
     * Signals background worker thread to re-open stream, avoiding audio thread deadlock.
     */
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

    // =========================================================================
    // Telemetry & Metrics (Lock-free thread-safe queries)
    // =========================================================================

    [[nodiscard]] float getPeakLevelLeft() const noexcept {
        return m_peakLeft.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float getPeakLevelRight() const noexcept {
        return m_peakRight.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t getFramesRendered() const noexcept {
        return m_framesRendered.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t getUnderrunCount() const noexcept {
        return m_underrunCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isPrerolling() const noexcept {
        return m_isPrerolling.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int32_t getActualBufferSize() const noexcept;
    [[nodiscard]] int32_t getFramesPerBurst() const noexcept;
    [[nodiscard]] int32_t getActualSampleRate() const noexcept;
    [[nodiscard]] int32_t getActualChannelCount() const noexcept;

    bool getLatestFftBands(float* outBands, size_t numBands) noexcept {
        return m_fftAnalyzer.getLatestBands(outBands, numBands);
    }

private:
    bool openStreamLocked();
    void closeStreamLocked();
    void restartWorkerLoop();

    SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& m_ringBuffer;
    DriftCompensator& m_compensator;
    FftAnalyzer m_fftAnalyzer;

    std::shared_ptr<oboe::AudioStream> m_stream;
    mutable std::mutex m_streamMutex; // Control-plane mutex only (never touched by onAudioReady)

    OboePlayerConfig m_config{};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_isPrerolling{true};
    std::atomic<uint16_t> m_activeChannels{2};

    // Telemetry atomics
    std::atomic<float> m_peakLeft{0.0f};
    std::atomic<float> m_peakRight{0.0f};
    std::atomic<uint64_t> m_framesRendered{0};
    std::atomic<uint64_t> m_underrunCount{0};

    // Asynchronous error recovery thread
    std::thread m_restartThread;
    std::mutex m_restartMutex;
    std::condition_variable m_restartCv;
    std::atomic<bool> m_restartPending{false};
    std::atomic<bool> m_stopWorker{false};
};

} // namespace vban
