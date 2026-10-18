#include "oboe_player.hpp"

#include <android/log.h>
#include <cmath>
#include <cstring>
#include <chrono>

#define LOG_TAG "OboePlayer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace vban {

OboePlayer::OboePlayer(SpscRingBuffer<float, VBAN_RING_BUFFER_CAPACITY>& ringBuffer,
                       DriftCompensator& compensator)
    : m_ringBuffer(ringBuffer),
      m_compensator(compensator) {
    m_restartThread = std::thread(&OboePlayer::restartWorkerLoop, this);
    LOGI("OboePlayer initialized with background error recovery worker");
}

OboePlayer::~OboePlayer() {
    stop();

    m_stopWorker.store(true, std::memory_order_release);
    m_restartCv.notify_all();
    if (m_restartThread.joinable()) {
        m_restartThread.join();
    }
    LOGI("OboePlayer destroyed");
}

bool OboePlayer::start(const OboePlayerConfig& config) {
    std::lock_guard<std::mutex> lock(m_streamMutex);

    if (m_playing.load(std::memory_order_acquire)) {
        LOGW("OboePlayer start called while already playing");
        return true;
    }

    m_config = config;
    m_activeChannels.store(config.channelCount, std::memory_order_release);

    if (!openStreamLocked()) {
        LOGE("Failed to open Oboe audio stream");
        return false;
    }

    oboe::Result res = m_stream->requestStart();
    if (res != oboe::Result::OK) {
        LOGE("Failed to start Oboe stream: %s", oboe::convertToText(res));
        closeStreamLocked();
        return false;
    }

    m_compensator.triggerFadeIn();
    m_playing.store(true, std::memory_order_release);
    LOGI("OboePlayer started: %u Hz, %u channels, device %d, Exclusive AAudio",
         config.sampleRate, config.channelCount, config.deviceId);
    return true;
}

void OboePlayer::stop() {
    std::lock_guard<std::mutex> lock(m_streamMutex);

    if (!m_playing.load(std::memory_order_acquire) && !m_stream) {
        return;
    }

    LOGI("Stopping OboePlayer...");
    m_playing.store(false, std::memory_order_release);

    closeStreamLocked();
    LOGI("OboePlayer stopped successfully");
}

bool OboePlayer::isPlaying() const noexcept {
    return m_playing.load(std::memory_order_acquire);
}

bool OboePlayer::reconfigure(uint32_t sampleRate, uint16_t channels, int32_t deviceId) {
    std::lock_guard<std::mutex> lock(m_streamMutex);

    if (m_config.sampleRate == sampleRate &&
        m_config.channelCount == channels &&
        m_config.deviceId == deviceId &&
        m_playing.load(std::memory_order_acquire)) {
        return true;
    }

    LOGI("Reconfiguring OboePlayer: %u -> %u Hz, %u -> %u ch, device %d -> %d",
         m_config.sampleRate, sampleRate, m_config.channelCount, channels, m_config.deviceId, deviceId);

    // 1. Soft 128-sample cosine fade-out on active stream
    if (m_playing.load(std::memory_order_acquire) && m_stream) {
        m_compensator.triggerFadeOut();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        closeStreamLocked();
    }

    // 2. Update configuration
    m_config.sampleRate = sampleRate;
    m_config.channelCount = channels;
    m_config.deviceId = deviceId;
    m_activeChannels.store(channels, std::memory_order_release);

    // 3. Reconfigure compensator & FFT analyzer
    m_compensator.reconfigure(sampleRate, channels);
    m_fftAnalyzer.configure(sampleRate);

    // 4. Open and start new stream
    if (!openStreamLocked()) {
        LOGE("Failed to open stream during reconfiguration");
        m_playing.store(false, std::memory_order_release);
        return false;
    }

    oboe::Result res = m_stream->requestStart();
    if (res != oboe::Result::OK) {
        LOGE("Failed to start stream during reconfiguration: %s", oboe::convertToText(res));
        closeStreamLocked();
        m_playing.store(false, std::memory_order_release);
        return false;
    }

    // 5. Soft 128-sample cosine fade-in on newly opened stream
    m_compensator.triggerFadeIn();
    m_playing.store(true, std::memory_order_release);
    LOGI("OboePlayer reconfiguration complete and playing");
    return true;
}

void OboePlayer::setAudioDevice(int32_t deviceId) {
    if (m_config.deviceId == deviceId) {
        return;
    }
    reconfigure(m_config.sampleRate, m_config.channelCount, deviceId);
}

bool OboePlayer::openStreamLocked() {
    oboe::AudioStreamBuilder builder;

    builder.setDirection(oboe::Direction::Output);
    builder.setAudioApi(m_config.audioApi);
    builder.setPerformanceMode(m_config.performanceMode);
    builder.setSharingMode(m_config.sharingMode);
    builder.setFormat(m_config.format);
    builder.setSampleRate(static_cast<int32_t>(m_config.sampleRate));
    builder.setChannelCount(static_cast<int32_t>(m_config.channelCount));

    if (m_config.deviceId > 0) {
        builder.setDeviceId(m_config.deviceId);
    }

    builder.setDataCallback(this);
    builder.setErrorCallback(this);

    oboe::Result result = builder.openStream(m_stream);
    if (result != oboe::Result::OK) {
        LOGW("Failed to open Exclusive AAudio stream (%s). Retrying with Shared mode...",
             oboe::convertToText(result));
        builder.setSharingMode(oboe::SharingMode::Shared);
        result = builder.openStream(m_stream);
        if (result != oboe::Result::OK) {
            LOGE("Failed to open AAudio stream in Shared mode: %s", oboe::convertToText(result));
            return false;
        }
    }

    const int32_t burst = m_stream->getFramesPerBurst();
    const int32_t optimalBufferSize = burst * 2;
    m_stream->setBufferSizeInFrames(optimalBufferSize);

    LOGI("Oboe stream opened: Rate=%d, Channels=%d, Format=%s, Burst=%d, BufferSize=%d, Capacity=%d",
         m_stream->getSampleRate(),
         m_stream->getChannelCount(),
         oboe::convertToText(m_stream->getFormat()),
         burst,
         m_stream->getBufferSizeInFrames(),
         m_stream->getBufferCapacityInFrames());

    return true;
}

void OboePlayer::closeStreamLocked() {
    if (m_stream) {
        m_stream->requestStop();
        m_stream->close();
        m_stream.reset();
    }
}

// =============================================================================
// Real-Time Audio Callback Engine (SCHED_FIFO)
// =============================================================================
oboe::DataCallbackResult OboePlayer::onAudioReady(oboe::AudioStream* /*stream*/,
                                                 void* audioData,
                                                 int32_t numFrames) {
    if (numFrames <= 0 || audioData == nullptr) [[unlikely]] {
        return oboe::DataCallbackResult::Continue;
    }

    float* const outBuffer = static_cast<float*>(audioData);
    const uint16_t channels = m_activeChannels.load(std::memory_order_relaxed);

    const size_t framesRendered = m_compensator.process(
        m_ringBuffer,
        outBuffer,
        static_cast<size_t>(numFrames),
        channels
    );

    if (framesRendered < static_cast<size_t>(numFrames)) [[unlikely]] {
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
    }

    float maxL = 0.0f;
    float maxR = 0.0f;

    if (channels >= 2) {
        for (int32_t i = 0; i < numFrames; ++i) {
            const float l = std::abs(outBuffer[i * channels + 0]);
            const float r = std::abs(outBuffer[i * channels + 1]);
            if (l > maxL) maxL = l;
            if (r > maxR) maxR = r;
        }
    } else {
        for (int32_t i = 0; i < numFrames; ++i) {
            const float m = std::abs(outBuffer[i]);
            if (m > maxL) maxL = m;
        }
        maxR = maxL;
    }

    // Fast attack with smooth exponential decay envelope across callbacks (~50ms hold)
    const float prevL = m_peakLeft.load(std::memory_order_relaxed);
    const float prevR = m_peakRight.load(std::memory_order_relaxed);
    constexpr float decayFactor = 0.95f;
    m_peakLeft.store(std::max(maxL, prevL * decayFactor), std::memory_order_relaxed);
    m_peakRight.store(std::max(maxR, prevR * decayFactor), std::memory_order_relaxed);
    m_framesRendered.fetch_add(static_cast<uint64_t>(numFrames), std::memory_order_relaxed);

    // Feed real-time FFT spectral visualizer (lock-free, zero-alloc)
    if (channels >= 2) {
        m_fftAnalyzer.pushStereo(outBuffer, static_cast<size_t>(numFrames));
    } else {
        m_fftAnalyzer.pushSamples(outBuffer, static_cast<size_t>(numFrames), 1);
    }

    return oboe::DataCallbackResult::Continue;
}

// =============================================================================
// Error Callbacks & Asynchronous Recovery Worker
// =============================================================================
void OboePlayer::onErrorBeforeClose(oboe::AudioStream* /*stream*/, oboe::Result error) {
    LOGW("OboePlayer::onErrorBeforeClose: error=%s (%d)",
         oboe::convertToText(error), static_cast<int>(error));
}

void OboePlayer::onErrorAfterClose(oboe::AudioStream* /*stream*/, oboe::Result error) {
    LOGW("OboePlayer::onErrorAfterClose: error=%s (%d). Triggering asynchronous restart worker...",
         oboe::convertToText(error), static_cast<int>(error));

    m_restartPending.store(true, std::memory_order_release);
    m_restartCv.notify_one();
}

void OboePlayer::restartWorkerLoop() {
    while (!m_stopWorker.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(m_restartMutex);
        m_restartCv.wait(lock, [this] {
            return m_restartPending.load(std::memory_order_acquire) ||
                   m_stopWorker.load(std::memory_order_acquire);
        });

        if (m_stopWorker.load(std::memory_order_acquire)) {
            break;
        }

        m_restartPending.store(false, std::memory_order_release);
        lock.unlock();

        std::lock_guard<std::mutex> streamLock(m_streamMutex);
        if (!m_playing.load(std::memory_order_acquire)) {
            continue;
        }

        LOGI("Asynchronous restart worker starting recovery loop...");
        closeStreamLocked();

        bool restarted = false;
        for (int attempt = 1; attempt <= 5 && !m_stopWorker.load(std::memory_order_acquire); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            LOGI("Recovery attempt %d of 5...", attempt);

            if (openStreamLocked()) {
                oboe::Result res = m_stream->requestStart();
                if (res == oboe::Result::OK) {
                    m_compensator.triggerFadeIn();
                    restarted = true;
                    LOGI("Stream successfully recovered on attempt %d", attempt);
                    break;
                }
                closeStreamLocked();
            }
        }

        if (!restarted) {
            LOGE("Stream recovery failed after 5 attempts. Setting player state to stopped.");
            m_playing.store(false, std::memory_order_release);
        }
    }
}

int32_t OboePlayer::getActualBufferSize() const noexcept {
    std::lock_guard<std::mutex> lock(m_streamMutex);
    return m_stream ? m_stream->getBufferSizeInFrames() : 0;
}

int32_t OboePlayer::getFramesPerBurst() const noexcept {
    std::lock_guard<std::mutex> lock(m_streamMutex);
    return m_stream ? m_stream->getFramesPerBurst() : 0;
}

int32_t OboePlayer::getActualSampleRate() const noexcept {
    std::lock_guard<std::mutex> lock(m_streamMutex);
    return m_stream ? m_stream->getSampleRate() : 0;
}

int32_t OboePlayer::getActualChannelCount() const noexcept {
    std::lock_guard<std::mutex> lock(m_streamMutex);
    return m_stream ? m_stream->getChannelCount() : 0;
}

} // namespace vban
