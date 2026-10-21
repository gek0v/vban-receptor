#include "vban_engine.hpp"

#include <android/log.h>
#include <cstring>
#include <array>
#include <span>
#include <bit>
#include <cmath>

#define LOG_TAG "VbanEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace vban {

VbanEngine& VbanEngine::getInstance() {
    static VbanEngine instance;
    return instance;
}

VbanEngine::VbanEngine() = default;

VbanEngine::~VbanEngine() {
    stop();
}

bool VbanEngine::initialize() {
    if (initialized_.load(std::memory_order_acquire)) {
        LOGI("VbanEngine already initialized");
        return true;
    }

    parser_.reset();
    ringBuffer_.reset();
    compensator_.reset();

    if (!player_) {
        player_ = std::make_unique<OboePlayer>(ringBuffer_, compensator_);
    }

    initialized_.store(true, std::memory_order_release);
    LOGI("VbanEngine initialized successfully");
    return true;
}

bool VbanEngine::isInitialized() const noexcept {
    return initialized_.load(std::memory_order_acquire);
}

bool VbanEngine::start(const std::string& listenIp, uint16_t port, const std::string& streamName, int32_t deviceId) {
    if (!isInitialized()) {
        if (!initialize()) {
            return false;
        }
    }

    if (running_.load(std::memory_order_acquire)) {
        LOGW("VbanEngine is already running");
        return true;
    }

    listenIp_ = listenIp;
    listenPort_ = port;
    targetStreamName_ = streamName;
    currentDeviceId_.store(deviceId, std::memory_order_relaxed);

    parser_.setTargetStreamName(targetStreamName_);
    parser_.reset();
    ringBuffer_.reset();
    compensator_.reset();

    LOGI("Starting VbanEngine: IP=%s, Port=%u, Stream='%s', DeviceId=%d",
         listenIp_.c_str(), listenPort_, targetStreamName_.c_str(), deviceId);

    // Launch decoupled stream reconfiguration worker
    m_stopReconfigWorker.store(false, std::memory_order_release);
    m_reconfigPending.store(false, std::memory_order_release);
    m_reconfigThread = std::thread(&VbanEngine::reconfigWorkerLoop, this);

    // Start Oboe playback pipeline
    if (player_) {
        OboePlayerConfig playerConfig;
        playerConfig.sampleRate = m_currentSampleRate.load(std::memory_order_acquire);
        playerConfig.channelCount = m_currentChannels.load(std::memory_order_acquire);
        playerConfig.deviceId = deviceId;
        player_->start(playerConfig);

        const int32_t burst = player_->getFramesPerBurst() > 0 ? player_->getFramesPerBurst() : 192;
        compensator_.setNetworkJitter(0.0f, static_cast<uint32_t>(burst));
    }

    // Launch high-priority UDP network receiver
    receiver_ = std::make_unique<VbanReceiver>(ringBuffer_, [this](uint32_t rate, uint16_t ch) {
        onStreamFormatChanged(rate, ch);
    });

    ReceiverSocketConfig netConfig;
    netConfig.listenIp = listenIp_;
    netConfig.port = listenPort_;
    netConfig.streamName = targetStreamName_;
    netConfig.socketBufferSize = 4 * 1024 * 1024;
    netConfig.receiveTimeoutMs = 100;

    if (!receiver_->start(netConfig)) {
        LOGE("Failed to start VbanReceiver on %s:%u", listenIp_.c_str(), listenPort_);
    }

    running_.store(true, std::memory_order_release);
    LOGI("VbanEngine started successfully");
    return true;
}

void VbanEngine::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    LOGI("Stopping VbanEngine...");

    // 1. Stop and destroy network receiver
    if (receiver_) {
        receiver_->stop();
        receiver_.reset();
    }

    // 2. Stop reconfiguration worker thread
    m_stopReconfigWorker.store(true, std::memory_order_release);
    m_reconfigCv.notify_all();
    if (m_reconfigThread.joinable()) {
        m_reconfigThread.join();
    }

    // 3. Stop Oboe audio player
    if (player_) {
        player_->stop();
    }

    parser_.reset();
    ringBuffer_.reset();
    compensator_.reset();
    LOGI("VbanEngine stopped");
}

bool VbanEngine::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void VbanEngine::setAudioDevice(int32_t deviceId) {
    currentDeviceId_.store(deviceId, std::memory_order_relaxed);
    LOGI("VbanEngine audio device set to %d", deviceId);
    if (player_) {
        player_->setAudioDevice(deviceId);
    }
}

void VbanEngine::onStreamFormatChanged(uint32_t newRate, uint16_t newChannels) {
    m_pendingSampleRate.store(newRate, std::memory_order_release);
    m_pendingChannels.store(newChannels, std::memory_order_release);
    m_reconfigPending.store(true, std::memory_order_release);
    m_reconfigCv.notify_one();
}

void VbanEngine::reconfigWorkerLoop() {
    while (!m_stopReconfigWorker.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(m_reconfigMutex);
        m_reconfigCv.wait(lock, [this] {
            return m_reconfigPending.load(std::memory_order_acquire) ||
                   m_stopReconfigWorker.load(std::memory_order_acquire);
        });

        if (m_stopReconfigWorker.load(std::memory_order_acquire)) {
            break;
        }

        m_reconfigPending.store(false, std::memory_order_release);
        const uint32_t newRate = m_pendingSampleRate.load(std::memory_order_acquire);
        const uint16_t newChannels = m_pendingChannels.load(std::memory_order_acquire);
        lock.unlock();

        LOGI("Asynchronous reconfiguration worker executing format switch: %u Hz, %u ch",
             newRate, newChannels);

        if (player_) {
            player_->reconfigure(newRate, newChannels, currentDeviceId_.load(std::memory_order_relaxed));
        }

        m_currentSampleRate.store(newRate, std::memory_order_release);
        m_currentChannels.store(newChannels, std::memory_order_release);
    }
}

bool VbanEngine::reconfigureStream(const AudioStreamConfig& config) {
    LOGI("VbanEngine reconfigureStream requested: %u Hz, %u ch, device %d",
         config.sampleRate, config.channelCount, config.targetDeviceId);
    m_currentSampleRate.store(config.sampleRate, std::memory_order_release);
    m_currentChannels.store(config.channelCount, std::memory_order_release);
    if (player_) {
        player_->reconfigure(config.sampleRate, config.channelCount, config.targetDeviceId);
    }
    return true;
}

NativeTelemetry VbanEngine::getTelemetry() const noexcept {
    NativeTelemetry telem{};

    const uint32_t defaultRate = m_currentSampleRate.load(std::memory_order_acquire);
    const uint16_t defaultCh = m_currentChannels.load(std::memory_order_acquire);

    if (receiver_ && receiver_->isRunning()) {
        const auto rxTelem = receiver_->getTelemetry();
        telem.totalPacketsReceived = rxTelem.totalPacketsReceived;
        telem.lostPackets = rxTelem.lostPackets;
        telem.jitterMs = rxTelem.jitterMs;
        telem.sampleRate = rxTelem.activeSampleRate ? rxTelem.activeSampleRate : defaultRate;
        telem.channelCount = rxTelem.activeChannels ? rxTelem.activeChannels : defaultCh;
        telem.bitDepth = rxTelem.activeBitDepth ? rxTelem.activeBitDepth : 24;
    } else {
        const auto parserTelem = parser_.getTelemetry();
        telem.totalPacketsReceived = parserTelem.totalPacketsReceived;
        telem.lostPackets = parserTelem.lostPackets;
        telem.jitterMs = static_cast<float>(parserTelem.jitterMs);
        telem.sampleRate = defaultRate;
        telem.channelCount = defaultCh;
        telem.bitDepth = 24;
    }

    telem.bufferOccupancyPercent = ringBuffer_.occupancyPercent();

    float peakL = 0.0f;
    float peakR = 0.0f;

    if (player_ && player_->isPlaying()) {
        peakL = player_->getPeakLevelLeft();
        peakR = player_->getPeakLevelRight();
    }
    if (receiver_) {
        peakL = std::max(peakL, receiver_->getPeakLeft());
        peakR = std::max(peakR, receiver_->getPeakRight());
    }

    telem.peakLevelLeft = peakL;
    telem.peakLevelRight = peakR;

    const int32_t burst = (player_ && player_->getFramesPerBurst() > 0) ? player_->getFramesPerBurst() : 192;
    compensator_.setNetworkJitter(telem.jitterMs, static_cast<uint32_t>(burst));
    compensator_.setUnrecoverableDrops(telem.lostPackets);

    telem.targetWatermarkMs = compensator_.getTargetWatermarkMs();
    telem.unrecoverableDrops = compensator_.getUnrecoverableDrops();
    telem.glitchCount = compensator_.getGlitchCount();

    return telem;
}

bool VbanEngine::processPacket(const uint8_t* buffer, size_t size) {
    if (receiver_) {
        return receiver_->processDatagram(buffer, size);
    }

    ParsedPacket packet;
    ParseResult res = parser_.parse(buffer, size, packet);
    if (res != ParseResult::Success) {
        return false;
    }

    m_currentSampleRate.store(packet.sampleRate, std::memory_order_release);
    m_currentChannels.store(packet.channels, std::memory_order_release);

    const size_t totalSamples = static_cast<size_t>(packet.samplesPerFrame) * packet.channels;

    alignas(64) static thread_local std::array<float, 4096> staging;
    if (totalSamples > staging.size()) {
        return false;
    }

    bool converted = convertVbanPayloadToFloat(
        static_cast<uint8_t>(packet.dataType),
        packet.payload,
        staging.data(),
        totalSamples
    );

    if (!converted) {
        return false;
    }

    ringBuffer_.write(staging.data(), totalSamples);

    const int32_t burst = (player_ && player_->getFramesPerBurst() > 0) ? player_->getFramesPerBurst() : 192;
    compensator_.setNetworkJitter(static_cast<float>(parser_.getTelemetry().jitterMs), static_cast<uint32_t>(burst));
    compensator_.setUnrecoverableDrops(parser_.getTelemetry().lostPackets);

    return true;
}

bool VbanEngine::setTargetLatencyMs(float latencyMs) {
    const uint32_t currentRate = m_currentSampleRate.load(std::memory_order_relaxed);
    compensator_.setTargetLatencyMs(latencyMs, currentRate > 0 ? currentRate : 48000);
    LOGI("VbanEngine target latency set to %.1f ms (rate %u Hz)", latencyMs, currentRate);
    return true;
}

bool VbanEngine::getLatestFftBands(float* outBands, size_t numBands) noexcept {
    if (player_) {
        return player_->getLatestFftBands(outBands, numBands);
    }
    return false;
}

} // namespace vban
