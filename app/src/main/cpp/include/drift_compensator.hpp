#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <concepts>
#include <ostream>

#include "spsc_ring_buffer.hpp"

namespace vban {

#ifndef VBAN_RING_BUFFER_CAPACITY_DEFINED
#define VBAN_RING_BUFFER_CAPACITY_DEFINED
inline constexpr size_t VBAN_RING_BUFFER_CAPACITY = 262144;
#endif

/**
 * @brief Configuration parameters for dynamic clock drift compensation.
 */
struct ClockDriftConfig {
    double timeConstantTau{8.0};           ///< EMA filter time constant in seconds (default ~8.0s)
    double targetOccupancyPercent{50.0};   ///< Target buffer occupancy percentage [0.0%, 100.0%]
    double kp{2.0};                        ///< Proportional gain in PPM per % error
    double ki{0.05};                       ///< Integral gain in PPM per (% error * sec)
    double maxDriftPpm{100.0};             ///< Hard bounds clamping (+-100.0 PPM)
    double maxSlewPpmPerSec{0.1};          ///< Maximum rate of change (<= 0.1 PPM/s for inaudible pitch glide)
    size_t ringBufferCapacity{262144};     ///< SPSC ring buffer capacity in frames
};

/**
 * @brief Thread-safe lock-free snapshot of clock drift compensator telemetry.
 */
struct ClockDriftTelemetry {
    float estimatedDriftPpm{0.0f};         ///< Current slew-limited drift correction in PPM
    float integralDriftPpm{0.0f};          ///< Steady-state crystal drift estimate from integrator (PPM)
    float currentRatio{1.0f};              ///< Current resampling ratio (1.0 + delta_ppm * 1e-6)
    float smoothedWatermark{0.0f};         ///< 2nd-order EMA filtered buffer fill level (frames)
    float smoothedOccupancyPercent{0.0f};  ///< 2nd-order EMA filtered occupancy percentage (0..100%)
    float rawOccupancyPercent{0.0f};       ///< Instantaneous raw occupancy percentage
};

/**
 * @brief 2nd-Order Critically Damped Exponential Moving Average (EMA) Jitter Filter.
 *
 * Implements two cascaded 1st-order EMA filters to achieve -40 dB/decade stopband
 * roll-off, attenuating Wi-Fi packet arrival burst jitter while preserving DC drift.
 */
class EmaFilter2ndOrder {
public:
    explicit EmaFilter2ndOrder(double tauSeconds = 8.0) noexcept
        : m_tau(std::max(0.001, tauSeconds)) {}

    void reset() noexcept {
        m_y1 = 0.0;
        m_y2 = 0.0;
        m_seeded = false;
    }

    void seed(double initialValue) noexcept {
        m_y1 = initialValue;
        m_y2 = initialValue;
        m_seeded = true;
    }

    double update(double x, double dt) noexcept {
        if (!m_seeded) [[unlikely]] {
            seed(x);
            return x;
        }

        if (dt <= 0.0) [[unlikely]] {
            return m_y2;
        }

        const double alpha = 1.0 - std::exp(-dt / m_tau);
        m_y1 += alpha * (x - m_y1);
        m_y2 += alpha * (m_y1 - m_y2);

        return m_y2;
    }

    [[nodiscard]] double getValue() const noexcept { return m_y2; }
    [[nodiscard]] double getFirstStageValue() const noexcept { return m_y1; }
    [[nodiscard]] bool isSeeded() const noexcept { return m_seeded; }
    void setTimeConstant(double tauSeconds) noexcept { m_tau = std::max(0.001, tauSeconds); }

private:
    double m_tau{8.0};
    double m_y1{0.0};
    double m_y2{0.0};
    bool m_seeded{false};
};

/**
 * @brief Proportional-Integral (PI) Servo Controller with Anti-Windup Clamping.
 */
class PiServoController {
public:
    explicit PiServoController(double kp = 2.0, double ki = 0.05, double maxPpm = 100.0) noexcept
        : m_kp(kp), m_ki(ki), m_maxPpm(std::abs(maxPpm)) {}

    void reset() noexcept {
        m_integral = 0.0;
        m_lastRawOutput = 0.0;
    }

    double update(double error, double dt) noexcept {
        if (dt <= 0.0) [[unlikely]] {
            return m_lastRawOutput;
        }

        const double p = m_kp * error;

        // Conditional integration (anti-windup back-calculation)
        const bool saturatingHigh = (m_lastRawOutput >= m_maxPpm && error > 0.0);
        const bool saturatingLow  = (m_lastRawOutput <= -m_maxPpm && error < 0.0);

        if (!saturatingHigh && !saturatingLow) {
            m_integral += m_ki * error * dt;
            m_integral = std::clamp(m_integral, -m_maxPpm, m_maxPpm);
        }

        m_lastRawOutput = std::clamp(p + m_integral, -m_maxPpm, m_maxPpm);
        return m_lastRawOutput;
    }

    [[nodiscard]] double getIntegral() const noexcept { return m_integral; }
    [[nodiscard]] double getLastOutput() const noexcept { return m_lastRawOutput; }
    void setGains(double kp, double ki) noexcept { m_kp = kp; m_ki = ki; }
    void setMaxPpm(double maxPpm) noexcept { m_maxPpm = std::abs(maxPpm); }

private:
    double m_kp{2.0};
    double m_ki{0.05};
    double m_maxPpm{100.0};
    double m_integral{0.0};
    double m_lastRawOutput{0.0};
};

/**
 * @brief Slew-Rate Limiter for Audiophile Pitch Stability (<= 0.1 PPM/s).
 */
class SlewRateLimiter {
public:
    explicit SlewRateLimiter(double maxSlewPerSec = 0.1, double maxAbsLimit = 100.0) noexcept
        : m_maxSlewPerSec(std::abs(maxSlewPerSec)), m_maxAbsLimit(std::abs(maxAbsLimit)) {}

    void reset() noexcept {
        m_currentValue = 0.0;
    }

    void seed(double initialValue) noexcept {
        m_currentValue = std::clamp(initialValue, -m_maxAbsLimit, m_maxAbsLimit);
    }

    double update(double targetValue, double dt) noexcept {
        if (dt <= 0.0) [[unlikely]] {
            return m_currentValue;
        }

        const double maxDelta = m_maxSlewPerSec * dt;
        const double delta = targetValue - m_currentValue;
        const double step = std::clamp(delta, -maxDelta, maxDelta);

        m_currentValue += step;
        m_currentValue = std::clamp(m_currentValue, -m_maxAbsLimit, m_maxAbsLimit);
        return m_currentValue;
    }

    [[nodiscard]] double getValue() const noexcept { return m_currentValue; }
    void setMaxSlewRate(double maxSlewPerSec) noexcept { m_maxSlewPerSec = std::abs(maxSlewPerSec); }
    void setMaxLimit(double maxLimit) noexcept { m_maxAbsLimit = std::abs(maxLimit); }

private:
    double m_maxSlewPerSec{0.1};
    double m_maxAbsLimit{100.0};
    double m_currentValue{0.0};
};

/**
 * @brief Audiophile Catmull-Rom Cubic Hermite Spline Micro-Resampler.
 *
 * Provides C^1 continuous micro-interpolation with zero dynamic allocation
 * and branch-free per-sample calculation.
 * Supports 1 to 8 channels, with specialized stereo branch-free path.
 */
class CatmullRomResampler {
public:
    static constexpr size_t MAX_CHANNELS = 8;
    static constexpr size_t STAGING_FRAMES = 1024;

    CatmullRomResampler() noexcept {
        reset();
    }

    ~CatmullRomResampler() = default;

    CatmullRomResampler(const CatmullRomResampler&) = delete;
    CatmullRomResampler& operator=(const CatmullRomResampler&) = delete;
    CatmullRomResampler(CatmullRomResampler&&) = delete;
    CatmullRomResampler& operator=(CatmullRomResampler&&) = delete;

    void reset() noexcept {
        unprime();
        m_totalInputFramesConsumed = 0;
        m_totalOutputFramesGenerated = 0;
        m_underrunFrames = 0;
    }

    void unprime() noexcept {
        for (size_t i = 0; i < 4; ++i) {
            for (size_t ch = 0; ch < MAX_CHANNELS; ++ch) {
                m_history[i][ch] = 0.0f;
            }
        }
        m_phase = 0.0;
        m_isPrimed = false;
        m_stagingAvailable = 0;
        m_stagingReadPos = 0;
    }

    /**
     * @brief Catmull-Rom cubic Hermite spline interpolation in Horner's factored form.
     * Evaluates in 3 FMAs per sample.
     */
    static inline float interpolateScalar(float p0, float p1, float p2, float p3, float t) noexcept {
        const float a0 = p1;
        const float a1 = 0.5f * (p2 - p0);
        const float a2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float a3 = -0.5f * p0 + 1.5f * (p1 - p2) + 0.5f * p3;
        return ((a3 * t + a2) * t + a1) * t + a0;
    }

    /**
     * @brief Stereo specialized evaluation with paired operations for vectorization.
     */
    static inline void interpolateStereo(
        const float p0[MAX_CHANNELS],
        const float p1[MAX_CHANNELS],
        const float p2[MAX_CHANNELS],
        const float p3[MAX_CHANNELS],
        float t,
        float out[2]
    ) noexcept {
        const float a0_l = p1[0];
        const float a0_r = p1[1];
        const float a1_l = 0.5f * (p2[0] - p0[0]);
        const float a1_r = 0.5f * (p2[1] - p0[1]);
        const float a2_l = p0[0] - 2.5f * p1[0] + 2.0f * p2[0] - 0.5f * p3[0];
        const float a2_r = p0[1] - 2.5f * p1[1] + 2.0f * p2[1] - 0.5f * p3[1];
        const float a3_l = -0.5f * p0[0] + 1.5f * (p1[0] - p2[0]) + 0.5f * p3[0];
        const float a3_r = -0.5f * p0[1] + 1.5f * (p1[1] - p2[1]) + 0.5f * p3[1];

        out[0] = ((a3_l * t + a2_l) * t + a1_l) * t + a0_l;
        out[1] = ((a3_r * t + a2_r) * t + a1_r) * t + a0_r;
    }

    template <size_t Capacity>
    size_t process(
        SpscRingBuffer<float, Capacity>& ringBuffer,
        float* output,
        size_t outputFrames,
        size_t channels,
        double ratio
    ) noexcept {
        if (output == nullptr || outputFrames == 0 || channels == 0 || channels > MAX_CHANNELS) [[unlikely]] {
            return 0;
        }

        const double clampedRatio = std::clamp(ratio, 0.5, 2.0);

        if (!m_isPrimed) [[unlikely]] {
            if (!primeFromRingBuffer(ringBuffer, channels)) {
                return 0;
            }
        }

        size_t outFrame = 0;

        if (channels == 2) {
            while (outFrame < outputFrames) {
                const float t = static_cast<float>(m_phase);
                interpolateStereo(
                    m_history[0], m_history[1], m_history[2], m_history[3],
                    t, &output[outFrame * 2]
                );
                outFrame++;
                m_phase += clampedRatio;

                while (m_phase >= 1.0) {
                    m_phase -= 1.0;
                    if (!advanceOneInputFrame(ringBuffer, channels)) {
                        m_underrunFrames += (outputFrames - outFrame);
                        m_totalOutputFramesGenerated += outFrame;
                        unprime();
                        return outFrame;
                    }
                }
            }
        } else {
            while (outFrame < outputFrames) {
                const float t = static_cast<float>(m_phase);
                for (size_t ch = 0; ch < channels; ++ch) {
                    output[outFrame * channels + ch] = interpolateScalar(
                        m_history[0][ch], m_history[1][ch],
                        m_history[2][ch], m_history[3][ch], t
                    );
                }
                outFrame++;
                m_phase += clampedRatio;

                while (m_phase >= 1.0) {
                    m_phase -= 1.0;
                    if (!advanceOneInputFrame(ringBuffer, channels)) {
                        m_underrunFrames += (outputFrames - outFrame);
                        m_totalOutputFramesGenerated += outFrame;
                        unprime();
                        return outFrame;
                    }
                }
            }
        }

        m_totalOutputFramesGenerated += outFrame;
        return outFrame;
    }

    size_t processDirect(
        const float* input,
        size_t inputFrames,
        size_t& inputFramesConsumed,
        float* output,
        size_t outputFrames,
        size_t channels,
        double ratio
    ) noexcept {
        if (!input || !output || inputFrames < 4 || outputFrames == 0 || channels == 0 || channels > MAX_CHANNELS) {
            return 0;
        }

        const double clampedRatio = std::clamp(ratio, 0.5, 2.0);

        if (!m_isPrimed) {
            for (size_t i = 0; i < 4; ++i) {
                for (size_t ch = 0; ch < channels; ++ch) {
                    m_history[i][ch] = input[i * channels + ch];
                }
            }
            m_phase = 0.0;
            m_isPrimed = true;
            inputFramesConsumed = 4;
        } else {
            inputFramesConsumed = 0;
        }

        size_t inIdx = inputFramesConsumed;
        size_t outFrame = 0;

        while (outFrame < outputFrames && inIdx < inputFrames) {
            const float t = static_cast<float>(m_phase);
            for (size_t ch = 0; ch < channels; ++ch) {
                output[outFrame * channels + ch] = interpolateScalar(
                    m_history[0][ch], m_history[1][ch],
                    m_history[2][ch], m_history[3][ch], t
                );
            }
            outFrame++;
            m_phase += clampedRatio;

            while (m_phase >= 1.0) {
                m_phase -= 1.0;
                if (inIdx < inputFrames) {
                    shiftHistory(&input[inIdx * channels], channels);
                    inIdx++;
                } else {
                    inputFramesConsumed = inIdx;
                    return outFrame;
                }
            }
        }

        inputFramesConsumed = inIdx;
        return outFrame;
    }

    [[nodiscard]] double getPhase() const noexcept { return m_phase; }
    [[nodiscard]] bool isPrimed() const noexcept { return m_isPrimed; }
    [[nodiscard]] uint64_t getTotalOutputFrames() const noexcept { return m_totalOutputFramesGenerated; }
    [[nodiscard]] uint64_t getUnderrunFrames() const noexcept { return m_underrunFrames; }

private:
    inline void shiftHistory(const float* nextFrame, size_t channels) noexcept {
        for (size_t ch = 0; ch < channels; ++ch) {
            m_history[0][ch] = m_history[1][ch];
            m_history[1][ch] = m_history[2][ch];
            m_history[2][ch] = m_history[3][ch];
            m_history[3][ch] = nextFrame[ch];
        }
        m_totalInputFramesConsumed++;
    }

    template <size_t Capacity>
    bool primeFromRingBuffer(SpscRingBuffer<float, Capacity>& ringBuffer, size_t channels) noexcept {
        const size_t availableElements = ringBuffer.availableRead();
        const size_t availableFrames = availableElements / channels;
        if (availableFrames == 0) {
            return false;
        }

        const size_t framesToRead = std::min(availableFrames, size_t{4});
        float initBlock[4 * MAX_CHANNELS];
        size_t readElements = ringBuffer.read(initBlock, framesToRead * channels);
        size_t actualFramesRead = readElements / channels;
        if (actualFramesRead == 0) {
            return false;
        }

        for (size_t i = 0; i < 4; ++i) {
            size_t srcIdx = std::min(i, actualFramesRead - 1);
            for (size_t ch = 0; ch < channels; ++ch) {
                m_history[i][ch] = initBlock[srcIdx * channels + ch];
            }
        }

        m_phase = 0.0;
        m_isPrimed = true;
        return true;
    }

    template <size_t Capacity>
    bool advanceOneInputFrame(SpscRingBuffer<float, Capacity>& ringBuffer, size_t channels) noexcept {
        if (m_stagingReadPos < m_stagingAvailable) {
            shiftHistory(&m_stagingBuffer[m_stagingReadPos * channels], channels);
            m_stagingReadPos++;
            return true;
        }

        const size_t availableElements = ringBuffer.availableRead();
        const size_t availableFrames = availableElements / channels;
        if (availableFrames == 0) {
            return false;
        }

        const size_t framesToRead = std::min(availableFrames, STAGING_FRAMES);
        const size_t elementsRead = ringBuffer.read(m_stagingBuffer.data(), framesToRead * channels);
        m_stagingAvailable = elementsRead / channels;
        m_stagingReadPos = 0;

        if (m_stagingAvailable > 0) {
            shiftHistory(&m_stagingBuffer[m_stagingReadPos * channels], channels);
            m_stagingReadPos++;
            return true;
        }

        return false;
    }

    alignas(64) float m_history[4][MAX_CHANNELS]{};
    double m_phase{0.0};
    bool m_isPrimed{false};

    alignas(64) std::array<float, STAGING_FRAMES * MAX_CHANNELS> m_stagingBuffer{};
    size_t m_stagingAvailable{0};
    size_t m_stagingReadPos{0};

    uint64_t m_totalInputFramesConsumed{0};
    uint64_t m_totalOutputFramesGenerated{0};
    uint64_t m_underrunFrames{0};
};

/**
 * @brief Stream transition states.
 */
enum class FadeState : uint8_t {
    Muted = 0,    ///< Signal gain is 0.0 (exact silence)
    FadingIn,     ///< Ramping gain from 0.0 to 1.0 (128 samples)
    Active,       ///< Signal gain is 1.0 (full audio pass-through)
    FadingOut     ///< Ramping gain from 1.0 to 0.0 (128 samples)
};

inline std::ostream& operator<<(std::ostream& os, FadeState state) {
    switch (state) {
        case FadeState::Muted: return os << "Muted";
        case FadeState::FadingIn: return os << "FadingIn";
        case FadeState::Active: return os << "Active";
        case FadeState::FadingOut: return os << "FadingOut";
    }
    return os << "Unknown";
}

/**
 * @brief 128-Sample Raised Cosine Stream Transition & Fade Engine.
 *
 * Implements w(n) = 0.5 * (1.0 - cos(pi * n / 128)) to eliminate pops,
 * clicks, and transient thumps during stream lifecycle events.
 */
class alignas(64) FadeEngine {
public:
    static constexpr size_t FADE_WINDOW_SIZE = 128;

    FadeEngine() noexcept {
        reset();
    }

    ~FadeEngine() = default;

    FadeEngine(const FadeEngine&) = delete;
    FadeEngine& operator=(const FadeEngine&) = delete;
    FadeEngine(FadeEngine&&) = delete;
    FadeEngine& operator=(FadeEngine&&) = delete;

    void reset() noexcept {
        m_state = FadeState::Muted;
        m_sampleIndex = 0;
    }

    void startFadeIn() noexcept {
        if (m_state == FadeState::Active) {
            return;
        }
        if (m_state == FadeState::FadingOut) {
            m_sampleIndex = (m_sampleIndex <= FADE_WINDOW_SIZE) ? (FADE_WINDOW_SIZE - m_sampleIndex) : 0;
        } else {
            m_sampleIndex = 0;
        }
        m_state = FadeState::FadingIn;
    }

    void startFadeOut() noexcept {
        if (m_state == FadeState::Muted) {
            return;
        }
        if (m_state == FadeState::FadingIn) {
            m_sampleIndex = (m_sampleIndex <= FADE_WINDOW_SIZE) ? (FADE_WINDOW_SIZE - m_sampleIndex) : 0;
        } else {
            m_sampleIndex = 0;
        }
        m_state = FadeState::FadingOut;
    }

    void setMuted() noexcept {
        m_state = FadeState::Muted;
        m_sampleIndex = FADE_WINDOW_SIZE;
    }

    void setActive() noexcept {
        m_state = FadeState::Active;
        m_sampleIndex = FADE_WINDOW_SIZE;
    }

    [[nodiscard]] FadeState getState() const noexcept { return m_state; }
    [[nodiscard]] bool isMuted() const noexcept { return m_state == FadeState::Muted; }
    [[nodiscard]] bool isActive() const noexcept { return m_state == FadeState::Active; }
    [[nodiscard]] bool isTransitioning() const noexcept {
        return m_state == FadeState::FadingIn || m_state == FadeState::FadingOut;
    }

    void process(float* buffer, size_t frames, size_t channels) noexcept;
    void emergencyFadeOut(float* buffer, size_t availableFrames, size_t totalFrames, size_t channels) noexcept;

    [[nodiscard]] static constexpr float getCosineWindow(size_t n) noexcept {
        return (n <= FADE_WINDOW_SIZE) ? s_cosineWindow[n] : 1.0f;
    }

    static const std::array<float, FADE_WINDOW_SIZE + 1> s_cosineWindow;

private:
    FadeState m_state{FadeState::Muted};
    size_t m_sampleIndex{0};
};

/**
 * @brief Dynamic Clock Drift Compensator Engine.
 *
 * Orchestrates the 2nd-order EMA jitter filter, PI servo controller,
 * slew-rate limiter, Catmull-Rom micro-resampler, and 128-sample cosine fade engine.
 */
class ClockDriftCompensator {
public:
    explicit ClockDriftCompensator(const ClockDriftConfig& config = ClockDriftConfig{}) noexcept;
    ~ClockDriftCompensator() = default;

    ClockDriftCompensator(const ClockDriftCompensator&) = delete;
    ClockDriftCompensator& operator=(const ClockDriftCompensator&) = delete;
    ClockDriftCompensator(ClockDriftCompensator&&) = delete;
    ClockDriftCompensator& operator=(ClockDriftCompensator&&) = delete;

    void reset() noexcept;

    void setTargetLatencyMs(float targetMs, uint32_t sampleRate) noexcept;
    void reconfigure(uint32_t sampleRate, uint16_t channels) noexcept;

    void triggerFadeIn() noexcept { m_fadeEngine.startFadeIn(); }
    void triggerFadeOut() noexcept { m_fadeEngine.startFadeOut(); }

    float update(size_t currentWatermarkFrames, double dt) noexcept;

    template <size_t Capacity>
    size_t process(
        SpscRingBuffer<float, Capacity>& ringBuffer,
        float* output,
        size_t outputFrames,
        size_t channels
    ) noexcept {
        if (output == nullptr || outputFrames == 0 || channels == 0 || channels > CatmullRomResampler::MAX_CHANNELS) [[unlikely]] {
            return 0;
        }

        const size_t availableElements = ringBuffer.availableRead();
        const size_t availableFrames = availableElements / channels;
        const double dt = (m_sampleRate > 0) ? (static_cast<double>(outputFrames) / m_sampleRate) : 0.005;

        const float ratio = update(availableFrames, dt);

        const size_t framesProduced = m_resampler.process(
            ringBuffer,
            output,
            outputFrames,
            channels,
            static_cast<double>(ratio)
        );

        if (framesProduced < outputFrames) [[unlikely]] {
            m_resampler.unprime();
            m_fadeEngine.emergencyFadeOut(output, framesProduced, outputFrames, channels);
        } else {
            if (m_fadeEngine.isMuted()) [[unlikely]] {
                m_fadeEngine.startFadeIn();
            }
            m_fadeEngine.process(output, outputFrames, channels);
        }

        return framesProduced;
    }

    [[nodiscard]] float getCurrentRatio() const noexcept {
        return m_telemetryCurrentRatio.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float getEstimatedDriftPpm() const noexcept {
        return m_telemetryDriftPpm.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float getSmoothedWatermark() const noexcept {
        return m_telemetrySmoothedWatermark.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float getSmoothedOccupancyPercent() const noexcept {
        return m_telemetrySmoothedPercent.load(std::memory_order_relaxed);
    }

    [[nodiscard]] ClockDriftTelemetry getTelemetry() const noexcept;
    [[nodiscard]] const ClockDriftConfig& getConfig() const noexcept { return m_config; }

    [[nodiscard]] FadeEngine& getFadeEngine() noexcept { return m_fadeEngine; }
    [[nodiscard]] const FadeEngine& getFadeEngine() const noexcept { return m_fadeEngine; }

    [[nodiscard]] CatmullRomResampler& getResampler() noexcept { return m_resampler; }
    [[nodiscard]] const CatmullRomResampler& getResampler() const noexcept { return m_resampler; }

private:
    ClockDriftConfig m_config;
    uint32_t m_sampleRate{48000};
    uint16_t m_channels{2};

    EmaFilter2ndOrder   m_jitterFilter;
    PiServoController   m_piServo;
    SlewRateLimiter     m_slewLimiter;
    CatmullRomResampler m_resampler;
    FadeEngine          m_fadeEngine;

    alignas(64) std::atomic<float> m_telemetryDriftPpm{0.0f};
    std::atomic<float> m_telemetryIntegralPpm{0.0f};
    std::atomic<float> m_telemetryCurrentRatio{1.0f};
    std::atomic<float> m_telemetrySmoothedWatermark{0.0f};
    std::atomic<float> m_telemetrySmoothedPercent{0.0f};
    std::atomic<float> m_telemetryRawPercent{0.0f};
};

using DriftCompensator = ClockDriftCompensator;

} // namespace vban
