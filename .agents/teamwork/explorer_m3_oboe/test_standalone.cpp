#include <iostream>
#include <vector>
#include <cmath>
#include <array>
#include <algorithm>
#include <cassert>
#include <iomanip>
#include <numeric>
#include <span>
#include <random>

constexpr double kPi = 3.14159265358979323846;

// ==============================================================================
// 1. Catmull-Rom Cubic Hermite Spline Micro-Resampler Formula
// ==============================================================================
inline float catmull_rom_sample(float p0, float p1, float p2, float p3, float t) {
    const float a0 = p1;
    const float a1 = 0.5f * (p2 - p0);
    const float a2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float a3 = -0.5f * p0 + 1.5f * (p1 - p2) + 0.5f * p3;
    return ((a3 * t + a2) * t + a1) * t + a0;
}

// ==============================================================================
// 2. Cosine Fade Lookup Table (128 samples)
// ==============================================================================
struct CosineFadeTable {
    static constexpr size_t kFadeLength = 128;
    std::array<float, kFadeLength + 1> inTable{};
    std::array<float, kFadeLength + 1> outTable{};

    CosineFadeTable() {
        for (size_t n = 0; n <= kFadeLength; ++n) {
            float w = 0.5f * (1.0f - std::cos(static_cast<float>(kPi * n / kFadeLength)));
            inTable[n] = w;
            outTable[n] = 1.0f - w;
        }
    }
};

// ==============================================================================
// 3. 2nd-Order Slow EMA Jitter Filter
// ==============================================================================
class SlowEmaFilter {
public:
    explicit SlowEmaFilter(float tau = 8.0f) : m_tau(tau) {}

    void reset(float initialValue = 131072.0f) {
        m_y1 = initialValue;
        m_y2 = initialValue;
        m_initialized = true;
    }

    float process(float x, float dt) {
        if (!m_initialized) {
            reset(x);
            return x;
        }
        float alpha = 1.0f - std::exp(-dt / m_tau);
        m_y1 = (1.0f - alpha) * m_y1 + alpha * x;
        m_y2 = (1.0f - alpha) * m_y2 + alpha * m_y1;
        return m_y2;
    }

    float value() const noexcept { return m_y2; }

private:
    float m_tau{8.0f};
    float m_y1{131072.0f};
    float m_y2{131072.0f};
    bool m_initialized{false};
};

// ==============================================================================
// 4. Adaptive PI Drift Servo Controller
// ==============================================================================
struct ServoConfig {
    float targetWatermark{131072.0f};
    float Kp{0.01f};               // Proportional gain (PPM per frame error)
    float Ki{0.0005f};             // Integral gain
    float maxIntegralPpm{50.0f};   // Anti-windup clamp
    float maxDriftPpm{100.0f};      // Hard clamp bounds [-100, +100] PPM
    float maxSlewPpmPerSec{0.1f};  // Audiophile slew rate clamp (<= 0.1 PPM/s)
};

class PiDriftServo {
public:
    using Config = ServoConfig;

    explicit PiDriftServo(const Config& cfg = Config{}) : m_cfg(cfg) {}

    void reset() {
        m_integral = 0.0f;
        m_currentPpm = 0.0f;
    }

    float update(float filteredWatermark, float dt) {
        float error = filteredWatermark - m_cfg.targetWatermark;

        // Integral accumulation with anti-windup
        m_integral += m_cfg.Ki * error * dt;
        m_integral = std::clamp(m_integral, -m_cfg.maxIntegralPpm, +m_cfg.maxIntegralPpm);

        // Proportional + Integral target
        float rawTargetPpm = m_cfg.Kp * error + m_integral;
        float clampedTarget = std::clamp(rawTargetPpm, -m_cfg.maxDriftPpm, +m_cfg.maxDriftPpm);

        // Slew rate limiting: enforce rate of change <= maxSlewPpmPerSec
        float maxStep = m_cfg.maxSlewPpmPerSec * dt;
        float delta = std::clamp(clampedTarget - m_currentPpm, -maxStep, +maxStep);
        m_currentPpm += delta;

        return 1.0f + m_currentPpm * 1e-6f;
    }

    float currentPpm() const noexcept { return m_currentPpm; }
    float currentRatio() const noexcept { return 1.0f + m_currentPpm * 1e-6f; }

private:
    Config m_cfg;
    float m_integral{0.0f};
    float m_currentPpm{0.0f};
};

int main() {
    std::cout << "=== Running Milestone 3 Mathematical Validation Suite ===" << std::endl;

    // 1. Catmull-Rom THD Verification across 48 kHz, 192 kHz, 768 kHz
    for (double fs : {48000.0, 192000.0, 768000.0}) {
        const double f = 1000.0;
        const double ratio = 1.00005; // +50 PPM drift
        const size_t N = 10000;
        std::vector<float> discrete(N + 10);
        for (size_t i = 0; i < discrete.size(); ++i) {
            discrete[i] = static_cast<float>(std::sin(2.0 * kPi * f * i / fs));
        }

        double energySignal = 0.0;
        double energyError = 0.0;
        double maxAbsError = 0.0;

        for (size_t n = 2; n < N - 2; ++n) {
            double t_virtual = n * ratio;
            int64_t k = static_cast<int64_t>(std::floor(t_virtual));
            float frac = static_cast<float>(t_virtual - k);

            float p0 = discrete[k - 1];
            float p1 = discrete[k];
            float p2 = discrete[k + 1];
            float p3 = discrete[k + 2];

            float resampled = catmull_rom_sample(p0, p1, p2, p3, frac);
            double analytic = std::sin(2.0 * kPi * f * t_virtual / fs);

            double err = resampled - analytic;
            if (std::abs(err) > maxAbsError) maxAbsError = std::abs(err);
            energySignal += analytic * analytic;
            energyError += err * err;
        }

        double thd_db = 10.0 * std::log10(energyError / energySignal);
        std::cout << "  [fs=" << std::setw(6) << static_cast<int>(fs) << " Hz] THD: "
                  << std::fixed << std::setprecision(2) << thd_db << " dB (Max Err: "
                  << std::scientific << maxAbsError << ")" << std::endl;
        assert(thd_db < -90.0);
    }

    // 2. Cosine Fade Table Verification
    CosineFadeTable fade;
    assert(std::abs(fade.inTable[0] - 0.0f) < 1e-6f);
    assert(std::abs(fade.inTable[128] - 1.0f) < 1e-6f);
    assert(std::abs(fade.outTable[0] - 1.0f) < 1e-6f);
    assert(std::abs(fade.outTable[128] - 0.0f) < 1e-6f);
    for (size_t n = 0; n <= 128; ++n) {
        float sum = fade.inTable[n] + fade.outTable[n];
        assert(std::abs(sum - 1.0f) < 1e-6f);
    }
    std::cout << "  [PASS] Cosine fade boundary values, symmetry, and complementarity" << std::endl;

    // 3. EMA Filter Wi-Fi Jitter Attenuation (> 99%)
    SlowEmaFilter filter(8.0f);
    filter.reset(131072.0f);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> jitterDist(-960.0f, +960.0f); // +/- 20ms burst jitter at 48k

    double sumRaw = 0.0, sumSqRaw = 0.0;
    double sumFilt = 0.0, sumSqFilt = 0.0;
    const size_t numPackets = 10000;
    const float dtPacket = 0.005f; // 5ms packet rate

    for (size_t i = 0; i < numPackets; ++i) {
        float raw = 131072.0f + jitterDist(rng);
        float filtered = filter.process(raw, dtPacket);

        sumRaw += raw; sumSqRaw += raw * raw;
        sumFilt += filtered; sumSqFilt += filtered * filtered;
    }

    double varRaw = (sumSqRaw - (sumRaw * sumRaw) / numPackets) / numPackets;
    double varFilt = (sumSqFilt - (sumFilt * sumFilt) / numPackets) / numPackets;
    double reduction = (1.0 - varFilt / varRaw) * 100.0;
    std::cout << "  [PASS] EMA Jitter Filter: Raw Var=" << varRaw << ", Filt Var=" << varFilt
              << ", Jitter Reduction: " << std::fixed << std::setprecision(3) << reduction << "%" << std::endl;
    assert(reduction > 99.0);

    // 4. PI Servo Convergence & Slew Rate (+35 PPM and -20 PPM)
    auto testDrift = [](float targetPpm) {
        PiDriftServo servo;
        SlowEmaFilter ema(8.0f);
        ema.reset(131072.0f);

        const float fs = 48000.0f;
        const float f_send = fs * (1.0f + targetPpm * 1e-6f);
        const float dt = 0.01f; // 10ms control interval
        float W = 131072.0f;

        float maxSlewObserved = 0.0f;
        float prevPpm = 0.0f;

        // Run 1000 seconds
        const size_t steps = 100000;
        for (size_t s = 0; s < steps; ++s) {
            float ratio = servo.currentRatio();
            float framesIn = f_send * dt;
            float framesOut = fs * ratio * dt;
            W += (framesIn - framesOut);

            float filteredW = ema.process(W, dt);
            servo.update(filteredW, dt);

            float deltaPpm = std::abs(servo.currentPpm() - prevPpm);
            float slew = deltaPpm / dt;
            if (slew > maxSlewObserved) maxSlewObserved = slew;
            prevPpm = servo.currentPpm();
        }

        std::cout << "  [PASS] Servo " << (targetPpm > 0 ? "+" : "") << targetPpm
                  << " PPM: Final PPM=" << std::fixed << std::setprecision(2) << servo.currentPpm()
                  << ", Buffer=" << W << ", Max Slew=" << maxSlewObserved << " PPM/s" << std::endl;
        assert(maxSlewObserved <= 0.1001f);
        assert(std::abs(W - 131072.0f) < 500.0f);
    };

    testDrift(+35.0f);
    testDrift(-20.0f);

    std::cout << "=== ALL VERIFICATION CHECKS PASSED SUCCESSFULLY ===" << std::endl;
    return 0;
}
