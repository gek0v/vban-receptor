#include "test_framework.hpp"
#include "drift_compensator.hpp"
#include "spsc_ring_buffer.hpp"

#include <vector>
#include <cmath>
#include <array>
#include <algorithm>
#include <numeric>
#include <random>

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// ==============================================================================
// 1. Catmull-Rom Cubic Hermite Spline: Pure Sine THD < -90 dB at 48k, 192k, 768k
//    across +-50 PPM and +-100 PPM drift
// ==============================================================================
TEST_CASE("drift_resampler_catmull_rom_sine_accuracy_thd_all_rates_and_ppm") {
    const std::array<double, 3> sampleRates = {48000.0, 192000.0, 768000.0};
    const std::array<double, 4> driftPpmList = {+50.0, -50.0, +100.0, -100.0};
    const double f = 1000.0;
    const size_t N = 8000;

    for (double fs : sampleRates) {
        for (double driftPpm : driftPpmList) {
            const double ratio = 1.0 + driftPpm * 1e-6;
            std::vector<float> discrete(N + 32);
            for (size_t i = 0; i < discrete.size(); ++i) {
                discrete[i] = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(i) / fs));
            }

            double energySignal = 0.0;
            double energyError = 0.0;

            for (size_t n = 8; n < N - 8; ++n) {
                double t_virtual = static_cast<double>(n) * ratio;
                int64_t k = static_cast<int64_t>(std::floor(t_virtual));
                float frac = static_cast<float>(t_virtual - static_cast<double>(k));

                float p0 = discrete[k - 1];
                float p1 = discrete[k];
                float p2 = discrete[k + 1];
                float p3 = discrete[k + 2];

                float resampled = vban::CatmullRomResampler::interpolateScalar(p0, p1, p2, p3, frac);
                double analytic = std::sin(2.0 * kPi * f * t_virtual / fs);

                double err = static_cast<double>(resampled) - analytic;
                energySignal += analytic * analytic;
                energyError += err * err;
            }

            double thd_db = 10.0 * std::log10(energyError / energySignal);
            ASSERT_LT(thd_db, -90.0);
        }
    }
}

// ==============================================================================
// 2. Catmull-Rom Exact Polynomial Preservation & Boundary Invariants
// ==============================================================================
TEST_CASE("drift_resampler_catmull_rom_polynomial_preservation") {
    // Constant signal preservation
    for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
        float v = vban::CatmullRomResampler::interpolateScalar(7.5f, 7.5f, 7.5f, 7.5f, t);
        ASSERT_NEAR(v, 7.5f, 1e-6f);
    }

    // Linear ramp preservation: f(x) = 10*x + 5
    for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
        float expected = 15.0f + 10.0f * t;
        float v = vban::CatmullRomResampler::interpolateScalar(5.0f, 15.0f, 25.0f, 35.0f, t);
        ASSERT_NEAR(v, expected, 1e-5f);
    }

    // Quadratic curve preservation: f(x) = (x+1)^2 => p0 = 0, p1 = 1, p2 = 4, p3 = 9
    for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
        float expected = (t + 1.0f) * (t + 1.0f);
        float v = vban::CatmullRomResampler::interpolateScalar(0.0f, 1.0f, 4.0f, 9.0f, t);
        ASSERT_NEAR(v, expected, 1e-5f);
    }

    // Boundary C0 continuity: p(0) == p1, p(1) == p2
    float v0 = vban::CatmullRomResampler::interpolateScalar(0.2f, 0.4f, 0.6f, 0.8f, 0.0f);
    float v1 = vban::CatmullRomResampler::interpolateScalar(0.2f, 0.4f, 0.6f, 0.8f, 1.0f);
    ASSERT_NEAR(v0, 0.4f, 1e-6f);
    ASSERT_NEAR(v1, 0.6f, 1e-6f);
}

// ==============================================================================
// 3. 128-Sample Raised Cosine Fade Windowing: Boundary, Symmetry, Complementarity
// ==============================================================================
TEST_CASE("drift_resampler_cosine_fade_boundary_and_symmetry") {
    // Exact boundaries
    ASSERT_NEAR(vban::FadeEngine::getCosineWindow(0), 0.0f, 1e-6f);
    ASSERT_NEAR(vban::FadeEngine::getCosineWindow(128), 1.0f, 1e-6f);

    // Exact midpoint
    ASSERT_NEAR(vban::FadeEngine::getCosineWindow(64), 0.5f, 1e-6f);

    // Strict monotonicity for fade-in
    for (size_t n = 0; n < 128; ++n) {
        ASSERT_GT(vban::FadeEngine::getCosineWindow(n + 1), vban::FadeEngine::getCosineWindow(n));
    }

    // Complementary symmetry: w(n) + w(128 - n) == 1.0
    for (size_t n = 0; n <= 128; ++n) {
        float sum = vban::FadeEngine::getCosineWindow(n) + vban::FadeEngine::getCosineWindow(128 - n);
        ASSERT_NEAR(sum, 1.0f, 1e-6f);
    }

    // Smooth boundary tangents (derivative approaches 0 at ends)
    float deltaStart = vban::FadeEngine::getCosineWindow(1) - vban::FadeEngine::getCosineWindow(0);
    float deltaEnd   = vban::FadeEngine::getCosineWindow(128) - vban::FadeEngine::getCosineWindow(127);
    ASSERT_LT(deltaStart, 0.001f);
    ASSERT_LT(deltaEnd, 0.001f);
}

// ==============================================================================
// 4. Fade Engine State Machine & Emergency Starvation
// ==============================================================================
TEST_CASE("drift_resampler_fade_engine_state_and_emergency") {
    vban::FadeEngine engine;
    ASSERT_EQ(engine.getState(), vban::FadeState::Muted);
    ASSERT_TRUE(engine.isMuted());

    // Fade-in over 128 samples
    engine.startFadeIn();
    ASSERT_EQ(engine.getState(), vban::FadeState::FadingIn);

    std::vector<float> audio(256 * 2, 1.0f);
    engine.process(audio.data(), 128, 2);
    ASSERT_EQ(engine.getState(), vban::FadeState::Active);
    ASSERT_TRUE(engine.isActive());

    // After fade-in completes, new audio passes through with unity gain
    std::fill(audio.begin(), audio.end(), 1.0f);
    engine.process(audio.data(), 64, 2);
    for (size_t i = 0; i < 64 * 2; ++i) {
        ASSERT_NEAR(audio[i], 1.0f, 1e-5f);
    }

    // Fade-out over 128 samples
    std::fill(audio.begin(), audio.end(), 1.0f);
    engine.startFadeOut();
    ASSERT_EQ(engine.getState(), vban::FadeState::FadingOut);
    engine.process(audio.data(), 128, 2);
    ASSERT_EQ(engine.getState(), vban::FadeState::Muted);
    ASSERT_TRUE(engine.isMuted());

    // Emergency starvation test
    std::vector<float> starvationBuf(256 * 2, 1.0f);
    engine.setActive();
    engine.emergencyFadeOut(starvationBuf.data(), 64, 256, 2);
    ASSERT_TRUE(engine.isMuted());

    // Frames beyond available (64..256) must be exact silence (0.0f)
    for (size_t i = 64 * 2; i < 256 * 2; ++i) {
        ASSERT_EQ(starvationBuf[i], 0.0f);
    }
    // Final available sample must be smoothly attenuated towards zero
    ASSERT_LT(starvationBuf[63 * 2], 0.1f);
}

// ==============================================================================
// 5. 2nd-Order Slow EMA Filter: Wi-Fi Jitter Variance Reduction (> 99%)
// ==============================================================================
TEST_CASE("drift_resampler_ema_jitter_attenuation_variance") {
    vban::EmaFilter2ndOrder filter(8.0);
    filter.reset();
    filter.seed(131072.0);

    std::mt19937 rng(1337);
    std::uniform_real_distribution<double> jitterDist(-960.0, +960.0); // +/- 20ms burst jitter at 48k

    const size_t numPackets = 10000;
    const double dtPacket = 0.005; // 5ms Wi-Fi packet arrival period

    double sumRaw = 0.0, sumSqRaw = 0.0;
    double sumFilt = 0.0, sumSqFilt = 0.0;

    for (size_t i = 0; i < numPackets; ++i) {
        double raw = 131072.0 + jitterDist(rng);
        double filtered = filter.update(raw, dtPacket);

        sumRaw += raw; sumSqRaw += raw * raw;
        sumFilt += filtered; sumSqFilt += filtered * filtered;
    }

    double varRaw = (sumSqRaw - (sumRaw * sumRaw) / numPackets) / numPackets;
    double varFilt = (sumSqFilt - (sumFilt * sumFilt) / numPackets) / numPackets;
    double reductionPercent = (1.0 - varFilt / varRaw) * 100.0;

    ASSERT_GT(reductionPercent, 99.0);
}

// ==============================================================================
// 6. PI Servo Dynamic Tracking & Convergence (+35 PPM Drift)
// ==============================================================================
TEST_CASE("drift_resampler_pi_servo_convergence_positive_drift") {
    vban::PiServoController pi(0.01, 0.0005, 100.0);
    vban::EmaFilter2ndOrder ema(8.0);
    vban::SlewRateLimiter slew(0.1, 100.0);

    ema.seed(131072.0);
    slew.seed(0.0);

    const double targetPpm = +35.0;
    const double fs = 48000.0;
    const double f_send = fs * (1.0 + targetPpm * 1e-6);
    const double dt = 0.01; // 10ms control step
    double W = 131072.0;

    double maxSlewObserved = 0.0;
    double prevPpm = 0.0;

    const size_t steps = 100000; // 1000 simulated seconds
    for (size_t s = 0; s < steps; ++s) {
        double ratio = 1.0 + slew.getValue() * 1e-6;
        double framesIn = f_send * dt;
        double framesOut = fs * ratio * dt;
        W += (framesIn - framesOut);

        double filteredW = ema.update(W, dt);
        double error = filteredW - 131072.0;
        double rawPpm = pi.update(error, dt);
        double currentPpm = slew.update(rawPpm, dt);

        double deltaPpm = std::abs(currentPpm - prevPpm);
        double rate = deltaPpm / dt;
        if (rate > maxSlewObserved) maxSlewObserved = rate;
        prevPpm = currentPpm;
    }

    ASSERT_LE(maxSlewObserved, 0.1001);
    ASSERT_NEAR(W, 131072.0, 500.0);
}

// ==============================================================================
// 7. PI Servo Dynamic Tracking & Convergence (-20 PPM Drift)
// ==============================================================================
TEST_CASE("drift_resampler_pi_servo_convergence_negative_drift") {
    vban::PiServoController pi(0.01, 0.0005, 100.0);
    vban::EmaFilter2ndOrder ema(8.0);
    vban::SlewRateLimiter slew(0.1, 100.0);

    ema.seed(131072.0);
    slew.seed(0.0);

    const double targetPpm = -20.0;
    const double fs = 48000.0;
    const double f_send = fs * (1.0 + targetPpm * 1e-6);
    const double dt = 0.01;
    double W = 131072.0;

    double maxSlewObserved = 0.0;
    double prevPpm = 0.0;

    const size_t steps = 100000;
    for (size_t s = 0; s < steps; ++s) {
        double ratio = 1.0 + slew.getValue() * 1e-6;
        double framesIn = f_send * dt;
        double framesOut = fs * ratio * dt;
        W += (framesIn - framesOut);

        double filteredW = ema.update(W, dt);
        double error = filteredW - 131072.0;
        double rawPpm = pi.update(error, dt);
        double currentPpm = slew.update(rawPpm, dt);

        double deltaPpm = std::abs(currentPpm - prevPpm);
        double rate = deltaPpm / dt;
        if (rate > maxSlewObserved) maxSlewObserved = rate;
        prevPpm = currentPpm;
    }

    ASSERT_LE(maxSlewObserved, 0.1001);
    ASSERT_NEAR(W, 131072.0, 500.0);
}

// ==============================================================================
// 8. PI Servo Anti-Windup Clamping & Slew Limiter Adherence
// ==============================================================================
TEST_CASE("drift_resampler_pi_anti_windup_and_slew_adherence") {
    vban::PiServoController pi(2.0, 0.05, 100.0);
    double dt = 0.1;
    double extremeError = 50.0; // Sustained +50% error

    for (int i = 0; i < 1000; ++i) {
        pi.update(extremeError, dt);
    }

    ASSERT_LE(pi.getIntegral(), 100.0);
    ASSERT_LE(pi.getLastOutput(), 100.0);
    ASSERT_GE(pi.getIntegral(), -100.0);
    ASSERT_GE(pi.getLastOutput(), -100.0);

    // Slew rate limiter adherence
    vban::SlewRateLimiter limiter(0.1, 100.0);
    limiter.seed(0.0);
    double stepChange = 50.0;

    for (int i = 0; i < 100; ++i) {
        double prev = limiter.getValue();
        double current = limiter.update(stepChange, 0.01);
        double stepDelta = std::abs(current - prev);
        ASSERT_LE(stepDelta, 0.001 + 1e-9);
    }
    ASSERT_NEAR(limiter.getValue(), 0.1, 1e-4);
}

// ==============================================================================
// 9. Stereo Branch & Multi-Channel Phase Coherence
// ==============================================================================
TEST_CASE("drift_resampler_multichannel_phase_coherence") {
    vban::CatmullRomResampler resampler;
    resampler.reset();

    const size_t kChannels = 8;
    std::vector<float> input(16 * kChannels);
    for (size_t f = 0; f < 16; ++f) {
        for (size_t c = 0; c < kChannels; ++c) {
            input[f * kChannels + c] = static_cast<float>(f * 10 + c);
        }
    }

    size_t consumed = 0;
    std::vector<float> output(10 * kChannels, 0.0f);
    size_t produced = resampler.processDirect(
        input.data(), 16, consumed, output.data(), 10, kChannels, 1.00005
    );

    ASSERT_EQ(produced, 10U);

    // Verify all channels remain strictly aligned with same phase progression
    for (size_t f = 0; f < produced; ++f) {
        float ch0 = output[f * kChannels + 0];
        for (size_t c = 1; c < kChannels; ++c) {
            float chC = output[f * kChannels + c];
            ASSERT_NEAR(chC - ch0, static_cast<float>(c), 1e-5f);
        }
    }
}

// ==============================================================================
// 10. ClockDriftCompensator Full Lifecycle & Ring Buffer Integration
// ==============================================================================
TEST_CASE("drift_compensator_ring_buffer_integration") {
    vban::ClockDriftConfig cfg;
    cfg.ringBufferCapacity = 1024;
    cfg.targetOccupancyPercent = 50.0;
    vban::ClockDriftCompensator comp(cfg);
    comp.reset();

    auto telem = comp.getTelemetry();
    ASSERT_NEAR(telem.estimatedDriftPpm, 0.0f, 1e-5f);
    ASSERT_NEAR(telem.currentRatio, 1.0f, 1e-6f);
    ASSERT_NEAR(telem.smoothedOccupancyPercent, 50.0f, 1e-4f);

    vban::SpscRingBuffer<float, 1024> ringBuffer;
    std::vector<float> testAudio(256 * 2, 0.5f);
    ringBuffer.write(testAudio.data(), testAudio.size());

    comp.triggerFadeIn();

    std::vector<float> outBuf(128 * 2, 0.0f);
    size_t rendered = comp.process(ringBuffer, outBuf.data(), 128, 2);
    ASSERT_EQ(rendered, 128U);

    telem = comp.getTelemetry();
    ASSERT_GT(telem.currentRatio, 0.999f);
    ASSERT_LT(telem.currentRatio, 1.001f);
}

// ==============================================================================
// 11. Starvation Underrun & Recovery: Clean Un-Mute and Resampler Re-Priming
// ==============================================================================
TEST_CASE("drift_resampler_starvation_recovery_unmutes_and_resumes") {
    vban::ClockDriftConfig cfg;
    cfg.ringBufferCapacity = 1024;
    cfg.targetOccupancyPercent = 50.0;
    vban::ClockDriftCompensator comp(cfg);
    comp.reset();

    vban::SpscRingBuffer<float, 1024> ringBuffer;
    const size_t kChannels = 2;
    const size_t kBlockFrames = 128;

    // --- Phase 1: Healthy Streaming (Active Playback) ---
    // Feed 259 frames of constant 0.8f audio into the ring buffer (4 for priming + 255 for 2 blocks at drift ratio).
    std::vector<float> healthyAudio(259 * kChannels, 0.8f);
    size_t written = ringBuffer.write(healthyAudio.data(), healthyAudio.size());
    ASSERT_EQ(written, healthyAudio.size());

    // Trigger initial fade-in and render first block
    comp.triggerFadeIn();
    std::vector<float> outBuf1(kBlockFrames * kChannels, 0.0f);
    size_t rendered1 = comp.process(ringBuffer, outBuf1.data(), kBlockFrames, kChannels);
    ASSERT_EQ(rendered1, kBlockFrames);
    ASSERT_TRUE(comp.getResampler().isPrimed());
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    // Fading in from 0.0 to 0.8: tail sample should be close to 0.8f
    ASSERT_GT(std::abs(outBuf1[(kBlockFrames - 1) * kChannels]), 0.5f);

    // Render second block: unity gain pass-through
    std::vector<float> outBuf2(kBlockFrames * kChannels, 0.0f);
    size_t rendered2 = comp.process(ringBuffer, outBuf2.data(), kBlockFrames, kChannels);
    ASSERT_EQ(rendered2, kBlockFrames);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_NEAR(outBuf2[i], 0.8f, 0.05f);
    }

    // --- Phase 2: Complete Starvation Dropout ---
    // Drain remaining frames to induce complete starvation dropout
    while (ringBuffer.availableRead() > 0) {
        float dummy[64];
        ringBuffer.read(dummy, std::min(ringBuffer.availableRead(), size_t{64}));
    }
    // Ring buffer is now completely empty (0 frames available).
    ASSERT_EQ(ringBuffer.availableRead(), 0U);

    std::vector<float> outBufStarve(kBlockFrames * kChannels, 1.0f);
    size_t renderedStarve = comp.process(ringBuffer, outBufStarve.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedStarve, 0U);

    // Emergency fade out MUST have muted the engine
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::Muted);

    // Resampler MUST be unprimed to prevent interpolating across dropout gap
    ASSERT_FALSE(comp.getResampler().isPrimed());

    // Buffer MUST be filled with exact silence (0.0f)
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_EQ(outBufStarve[i], 0.0f);
    }

    // Secondary starvation burst while still empty: remains muted & unprimed
    std::vector<float> outBufStarve2(kBlockFrames * kChannels, 1.0f);
    size_t renderedStarve2 = comp.process(ringBuffer, outBufStarve2.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedStarve2, 0U);
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    ASSERT_FALSE(comp.getResampler().isPrimed());
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_EQ(outBufStarve2[i], 0.0f);
    }

    // --- Phase 3: Buffer Refill & Recovery (The Critical Un-Mute Test) ---
    // Refill ring buffer with 259 frames of fresh audio (0.75f) for priming (4) and 2 blocks (255).
    std::vector<float> recoveredAudio(259 * kChannels, 0.75f);
    written = ringBuffer.write(recoveredAudio.data(), recoveredAudio.size());
    ASSERT_EQ(written, recoveredAudio.size());

    // Next audio callback: healthy frames resume.
    // ClockDriftCompensator MUST un-mute automatically and start fade-in!
    std::vector<float> outBufRecover1(kBlockFrames * kChannels, 0.0f);
    size_t renderedRecover1 = comp.process(ringBuffer, outBufRecover1.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedRecover1, kBlockFrames);

    // Resampler MUST have re-primed cleanly from fresh contiguous frames
    ASSERT_TRUE(comp.getResampler().isPrimed());

    // FadeEngine MUST have un-muted (not Muted!) and transitioned to Active across 128 samples
    ASSERT_FALSE(comp.getFadeEngine().isMuted());
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::Active);

    // Audio MUST start smoothly at 0.0f (cosine window start) and rise to 0.75f
    ASSERT_NEAR(outBufRecover1[0], 0.0f, 1e-4f);
    ASSERT_NEAR(outBufRecover1[1], 0.0f, 1e-4f);
    // Midpoint sample (index 64) should be ~ half amplitude (0.75 * 0.5 = 0.375)
    ASSERT_GT(outBufRecover1[64 * kChannels], 0.2f);
    // Final sample in window (index 127) should be near full amplitude
    ASSERT_NEAR(outBufRecover1[(kBlockFrames - 1) * kChannels], 0.75f, 0.05f);

    // --- Phase 4: Subsequent Blocks (Full Unity Gain Audio) ---
    std::vector<float> outBufRecover2(kBlockFrames * kChannels, 0.0f);
    size_t renderedRecover2 = comp.process(ringBuffer, outBufRecover2.data(), kBlockFrames, kChannels);
    ASSERT_EQ(renderedRecover2, kBlockFrames);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    for (size_t i = 0; i < kBlockFrames * kChannels; ++i) {
        ASSERT_NEAR(outBufRecover2[i], 0.75f, 0.05f);
    }

    // --- Phase 5: Partial Block (Sub-128 Frame) Recovery Multi-Step Transition ---
    // Drain ring buffer to test multi-step transition with 64-frame callbacks
    while (ringBuffer.availableRead() > 0) {
        float dummy[64];
        ringBuffer.read(dummy, std::min(ringBuffer.availableRead(), size_t{64}));
    }
    ASSERT_EQ(ringBuffer.availableRead(), 0U);

    std::vector<float> outBufStarve3(64 * kChannels, 1.0f);
    size_t renderedStarve3 = comp.process(ringBuffer, outBufStarve3.data(), 64, kChannels);
    ASSERT_EQ(renderedStarve3, 0U);
    ASSERT_TRUE(comp.getFadeEngine().isMuted());
    ASSERT_FALSE(comp.getResampler().isPrimed());

    // Refill buffer with audio (0.6f)
    std::vector<float> subBlockAudio(512 * kChannels, 0.6f);
    ringBuffer.write(subBlockAudio.data(), subBlockAudio.size());

    // Callback 1 (64 frames): should transition to FadingIn and remain FadingIn (halfway)
    std::vector<float> outBuf64_1(64 * kChannels, 0.0f);
    size_t rendered64_1 = comp.process(ringBuffer, outBuf64_1.data(), 64, kChannels);
    ASSERT_EQ(rendered64_1, 64U);
    ASSERT_TRUE(comp.getResampler().isPrimed());
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::FadingIn);
    ASSERT_TRUE(comp.getFadeEngine().isTransitioning());
    ASSERT_NEAR(outBuf64_1[0], 0.0f, 1e-4f);

    // Callback 2 (64 frames): completes the 128-sample cosine window -> becomes Active
    std::vector<float> outBuf64_2(64 * kChannels, 0.0f);
    size_t rendered64_2 = comp.process(ringBuffer, outBuf64_2.data(), 64, kChannels);
    ASSERT_EQ(rendered64_2, 64U);
    ASSERT_EQ(comp.getFadeEngine().getState(), vban::FadeState::Active);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    // End of block 2 is at full amplitude
    ASSERT_NEAR(outBuf64_2[(64 - 1) * kChannels], 0.6f, 0.05f);

    // Callback 3 (64 frames): full amplitude unity gain
    std::vector<float> outBuf64_3(64 * kChannels, 0.0f);
    size_t rendered64_3 = comp.process(ringBuffer, outBuf64_3.data(), 64, kChannels);
    ASSERT_EQ(rendered64_3, 64U);
    ASSERT_TRUE(comp.getFadeEngine().isActive());
    for (size_t i = 0; i < 64 * kChannels; ++i) {
        ASSERT_NEAR(outBuf64_3[i], 0.6f, 0.05f);
    }
}

