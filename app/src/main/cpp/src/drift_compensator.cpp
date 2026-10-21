#include "drift_compensator.hpp"

namespace vban {

const std::array<float, FadeEngine::FADE_WINDOW_SIZE + 1> FadeEngine::s_cosineWindow = {
    0.00000000f, 0.00015059f, 0.00060227f, 0.00135477f, 0.00240764f, 0.00376023f, 0.00541175f, 0.00736118f,
    0.00960736f, 0.01214893f, 0.01498437f, 0.01811197f, 0.02152983f, 0.02523591f, 0.02922797f, 0.03350360f,
    0.03806023f, 0.04289512f, 0.04800535f, 0.05338785f, 0.05903937f, 0.06495650f, 0.07113569f, 0.07757322f,
    0.08426519f, 0.09120759f, 0.09839623f, 0.10582679f, 0.11349477f, 0.12139558f, 0.12952444f, 0.13787646f,
    0.14644661f, 0.15522973f, 0.16422052f, 0.17341358f, 0.18280336f, 0.19238420f, 0.20215035f, 0.21209590f,
    0.22221488f, 0.23250119f, 0.24294863f, 0.25355090f, 0.26430163f, 0.27519434f, 0.28622245f, 0.29737934f,
    0.30865828f, 0.32005248f, 0.33155507f, 0.34315913f, 0.35485766f, 0.36664362f, 0.37850991f, 0.39044938f,
    0.40245484f, 0.41451906f, 0.42663476f, 0.43879466f, 0.45099143f, 0.46321772f, 0.47546616f, 0.48772939f,
    0.50000000f, 0.51227061f, 0.52453384f, 0.53678228f, 0.54900857f, 0.56120534f, 0.57336524f, 0.58548094f,
    0.59754516f, 0.60955062f, 0.62149009f, 0.63335638f, 0.64514234f, 0.65684087f, 0.66844493f, 0.67994752f,
    0.69134172f, 0.70262066f, 0.71377755f, 0.72480566f, 0.73569837f, 0.74644910f, 0.75705137f, 0.76749881f,
    0.77778512f, 0.78790410f, 0.79784965f, 0.80761580f, 0.81719664f, 0.82658642f, 0.83577948f, 0.84477027f,
    0.85355339f, 0.86212354f, 0.87047556f, 0.87860442f, 0.88650523f, 0.89417321f, 0.90160377f, 0.90879241f,
    0.91573481f, 0.92242678f, 0.92886431f, 0.93504350f, 0.94096063f, 0.94661215f, 0.95199465f, 0.95710488f,
    0.96193977f, 0.96649640f, 0.97077203f, 0.97476409f, 0.97847017f, 0.98188803f, 0.98501563f, 0.98785107f,
    0.99039264f, 0.99263882f, 0.99458825f, 0.99623977f, 0.99759236f, 0.99864523f, 0.99939773f, 0.99984941f,
    1.00000000f
};

void FadeEngine::process(float* buffer, size_t frames, size_t channels) noexcept {
    if (buffer == nullptr || frames == 0 || channels == 0) [[unlikely]] {
        return;
    }

    if (m_state == FadeState::Active) [[likely]] {
        return;
    }

    if (m_state == FadeState::Muted) {
        std::memset(buffer, 0, frames * channels * sizeof(float));
        return;
    }

    size_t frame = 0;
    if (m_state == FadeState::FadingIn) {
        while (frame < frames && m_sampleIndex < FADE_WINDOW_SIZE) {
            const float gain = s_cosineWindow[m_sampleIndex++];
            for (size_t ch = 0; ch < channels; ++ch) {
                buffer[frame * channels + ch] *= gain;
            }
            frame++;
        }
        if (m_sampleIndex >= FADE_WINDOW_SIZE) {
            m_state = FadeState::Active;
        }
    } else if (m_state == FadeState::FadingOut) {
        while (frame < frames && m_sampleIndex < FADE_WINDOW_SIZE) {
            const float gain = s_cosineWindow[FADE_WINDOW_SIZE - m_sampleIndex++];
            for (size_t ch = 0; ch < channels; ++ch) {
                buffer[frame * channels + ch] *= gain;
            }
            frame++;
        }
        if (m_sampleIndex >= FADE_WINDOW_SIZE) {
            m_state = FadeState::Muted;
            if (frame < frames) {
                std::memset(&buffer[frame * channels], 0, (frames - frame) * channels * sizeof(float));
            }
        }
    }
}

void FadeEngine::emergencyFadeOut(float* buffer, size_t availableFrames, size_t totalFrames, size_t channels) noexcept {
    if (buffer == nullptr || channels == 0) [[unlikely]] {
        return;
    }

    if (availableFrames > 0) {
        const size_t fadeSpan = std::min(availableFrames, FADE_WINDOW_SIZE);
        const size_t startOffset = availableFrames - fadeSpan;

        for (size_t i = 0; i < fadeSpan; ++i) {
            const size_t windowIdx = (i * FADE_WINDOW_SIZE) / fadeSpan;
            const float gain = s_cosineWindow[FADE_WINDOW_SIZE - windowIdx];
            const size_t f = startOffset + i;
            for (size_t ch = 0; ch < channels; ++ch) {
                buffer[f * channels + ch] *= gain;
            }
        }
    }

    if (totalFrames > availableFrames) {
        std::memset(&buffer[availableFrames * channels], 0, (totalFrames - availableFrames) * channels * sizeof(float));
    }

    m_state = FadeState::Muted;
    m_sampleIndex = FADE_WINDOW_SIZE;
}

ClockDriftCompensator::ClockDriftCompensator(const ClockDriftConfig& config) noexcept
    : m_config(config),
      m_jitterFilter(config.timeConstantTau),
      m_piServo(config.kp, config.ki, config.maxDriftPpm),
      m_slewLimiter(config.maxSlewPpmPerSec, config.maxDriftPpm) {
    reset();
}

void ClockDriftCompensator::reset() noexcept {
    m_jitterFilter.reset();
    m_piServo.reset();
    m_slewLimiter.reset();
    m_resampler.reset();
    m_fadeEngine.reset();

    const float targetPct = static_cast<float>(m_config.targetOccupancyPercent);
    const float targetFrames = static_cast<float>(
        m_config.ringBufferCapacity * (m_config.targetOccupancyPercent * 0.01)
    );

    m_telemetryDriftPpm.store(0.0f, std::memory_order_relaxed);
    m_telemetryIntegralPpm.store(0.0f, std::memory_order_relaxed);
    m_telemetryCurrentRatio.store(1.0f, std::memory_order_relaxed);
    m_telemetrySmoothedWatermark.store(targetFrames, std::memory_order_relaxed);
    m_telemetrySmoothedPercent.store(targetPct, std::memory_order_relaxed);
    m_telemetryRawPercent.store(targetPct, std::memory_order_relaxed);

    m_glitchCount.store(0, std::memory_order_relaxed);
    m_unrecoverableDrops.store(0, std::memory_order_relaxed);
    m_targetOccupancyPercent.store(m_config.targetOccupancyPercent, std::memory_order_relaxed);
    const float baseMs = m_baseTargetLatencyMs.load(std::memory_order_relaxed);
    m_targetWatermarkMs.store(baseMs > 0.0f ? baseMs : 5.0f, std::memory_order_relaxed);
}

void ClockDriftCompensator::setTargetLatencyMs(float targetMs, uint32_t sampleRate) noexcept {
    if (sampleRate == 0 || targetMs <= 0.0f) {
        return;
    }
    m_sampleRate = sampleRate;
    m_baseTargetLatencyMs.store(targetMs, std::memory_order_relaxed);
    m_targetWatermarkMs.store(targetMs, std::memory_order_relaxed);
    const double targetFrames = (static_cast<double>(targetMs) * 0.001) * sampleRate;
    const double targetPct = (targetFrames / static_cast<double>(m_config.ringBufferCapacity)) * 100.0;
    const double clampedPct = std::clamp(targetPct, 0.01, 95.0);
    m_targetOccupancyPercent.store(clampedPct, std::memory_order_relaxed);
    m_config.targetOccupancyPercent = clampedPct;
}

void ClockDriftCompensator::setNetworkJitter(float jitterMs, uint32_t burstFrames) noexcept {
    const float sr = (m_sampleRate > 0) ? static_cast<float>(m_sampleRate) : 48000.0f;
    const float burstMs = (burstFrames > 0) ? (static_cast<float>(burstFrames) * 1000.0f / sr) : 4.0f;
    const float baseMs = m_baseTargetLatencyMs.load(std::memory_order_relaxed);
    float targetMs = std::max(baseMs, burstMs + 2.5f * jitterMs);
    targetMs = std::clamp(targetMs, std::max(5.0f, baseMs), std::max(25.0f, baseMs));
    m_targetWatermarkMs.store(targetMs, std::memory_order_relaxed);

    const double targetFrames = (static_cast<double>(targetMs) * 0.001) * static_cast<double>(sr);
    const double targetPct = (targetFrames / static_cast<double>(m_config.ringBufferCapacity)) * 100.0;
    const double clampedPct = std::clamp(targetPct, 0.01, 95.0);
    m_targetOccupancyPercent.store(clampedPct, std::memory_order_relaxed);
    m_config.targetOccupancyPercent = clampedPct;
}

void ClockDriftCompensator::reconfigure(uint32_t sampleRate, uint16_t channels) noexcept {
    m_sampleRate = (sampleRate > 0) ? sampleRate : 48000;
    m_channels = (channels > 0) ? channels : 2;
    reset();
}

float ClockDriftCompensator::update(size_t currentWatermarkFrames, double dt) noexcept {
    if (dt <= 0.0) [[unlikely]] {
        return m_telemetryCurrentRatio.load(std::memory_order_relaxed);
    }

    const double cap = static_cast<double>(m_config.ringBufferCapacity);
    const double rawFrames = static_cast<double>(currentWatermarkFrames);
    const double rawPercent = (rawFrames / cap) * 100.0;

    // 1. Pass raw watermark through 2nd-order slow EMA jitter filter
    const double filteredFrames = m_jitterFilter.update(rawFrames, dt);
    const double filteredPercent = (filteredFrames / cap) * 100.0;

    // 2. Compute error relative to target watermark
    const double errorPercent = filteredPercent - m_targetOccupancyPercent.load(std::memory_order_relaxed);

    // 3. Update PI servo controller with anti-windup clamping
    const double rawCorrectionPpm = m_piServo.update(errorPercent, dt);

    // 4. Enforce audiophile slew-rate limiter (<= 0.1 PPM/s)
    const double slewLimitedPpm = m_slewLimiter.update(rawCorrectionPpm, dt);

    // 5. Compute resampling ratio: 1.0 + delta_ppm * 1e-6
    const double ratio = 1.0 + (slewLimitedPpm * 1e-6);
    const float finalRatio = static_cast<float>(ratio);

    // 6. Publish non-blocking telemetry snapshots via atomics
    m_telemetryDriftPpm.store(static_cast<float>(slewLimitedPpm), std::memory_order_relaxed);
    m_telemetryIntegralPpm.store(static_cast<float>(m_piServo.getIntegral()), std::memory_order_relaxed);
    m_telemetryCurrentRatio.store(finalRatio, std::memory_order_relaxed);
    m_telemetrySmoothedWatermark.store(static_cast<float>(filteredFrames), std::memory_order_relaxed);
    m_telemetrySmoothedPercent.store(static_cast<float>(filteredPercent), std::memory_order_relaxed);
    m_telemetryRawPercent.store(static_cast<float>(rawPercent), std::memory_order_relaxed);

    return finalRatio;
}

ClockDriftTelemetry ClockDriftCompensator::getTelemetry() const noexcept {
    ClockDriftTelemetry t;
    t.estimatedDriftPpm = m_telemetryDriftPpm.load(std::memory_order_relaxed);
    t.integralDriftPpm = m_telemetryIntegralPpm.load(std::memory_order_relaxed);
    t.currentRatio = m_telemetryCurrentRatio.load(std::memory_order_relaxed);
    t.smoothedWatermark = m_telemetrySmoothedWatermark.load(std::memory_order_relaxed);
    t.smoothedOccupancyPercent = m_telemetrySmoothedPercent.load(std::memory_order_relaxed);
    t.rawOccupancyPercent = m_telemetryRawPercent.load(std::memory_order_relaxed);
    return t;
}

} // namespace vban
