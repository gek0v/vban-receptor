#include "test_framework.hpp"
#include "fft_analyzer.hpp"

#include <cmath>
#include <vector>
#include <array>
#include <thread>
#include <atomic>
#include <algorithm>
#include <numeric>

using namespace vban;

// ==============================================================================
// 1. Hann Window Symmetry and Normalization Tests
// ==============================================================================
TEST_CASE("fft_analyzer_hann_window_symmetry") {
    FftAnalyzer analyzer(48000, 64);
    const auto& window = analyzer.getWindow();

    ASSERT_EQ(window.size(), FftAnalyzer::kFftSize);

    // Verify window bounds are zero (or near zero)
    ASSERT_NEAR(window[0], 0.0f, 1e-5f);
    ASSERT_NEAR(window[FftAnalyzer::kFftSize - 1], 0.0f, 1e-5f);

    // Verify strict symmetry: w[n] == w[N - 1 - n]
    for (size_t n = 0; n < FftAnalyzer::kFftSize / 2; ++n) {
        float left = window[n];
        float right = window[FftAnalyzer::kFftSize - 1 - n];
        ASSERT_NEAR(left, right, 1e-6f);
    }

    // Verify peak is at midpoint (n = 511 / 512 for 1024-point)
    float midLeft = window[511];
    float midRight = window[512];
    ASSERT_NEAR(midLeft, midRight, 1e-5f);
    ASSERT_GT(midLeft, 0.999f);

    // Coherent gain sum for Hann is approx (N - 1) / 2 = 511.5
    float sum = analyzer.getWindowSum();
    ASSERT_NEAR(sum, 511.5f, 1.0f);
}

// ==============================================================================
// 2. DC (0 Hz) Accuracy Test
// ==============================================================================
TEST_CASE("fft_analyzer_dc_accuracy") {
    FftAnalyzer analyzer(48000, 64);

    // Pure DC signal with amplitude 1.0
    std::array<float, FftAnalyzer::kFftSize> dcSignal{};
    dcSignal.fill(1.0f);

    // 1. Unwindowed raw FFT: exact energy isolation at DC bin
    std::array<Complex, FftAnalyzer::kFftSize> rawSpectrum{};
    analyzer.computeFftRaw(dcSignal.data(), rawSpectrum.data(), false);

    // Bin 0 must contain entire energy: 1024.0
    ASSERT_NEAR(rawSpectrum[0].magnitude() / 1024.0f, 1.0f, 1e-4f);
    for (size_t k = 1; k < FftAnalyzer::kSpectrumBins; ++k) {
        ASSERT_NEAR(rawSpectrum[k].magnitude(), 0.0f, 1e-4f);
    }

    // 2. Windowed real-time processing & display bands
    std::array<float, 64> bands{};
    analyzer.processDirect(dcSignal.data(), bands.data(), 64);
    const auto& magnitudes = analyzer.getLatestMagnitudes();

    // DC bin is normalized to 1.0
    ASSERT_NEAR(magnitudes[0], 1.0f, 0.02f);

    // Band 0 covers DC and lowest bass, so it must capture the DC peak
    ASSERT_NEAR(bands[0], 1.0f, 0.02f);

    // Higher audio bands (>200 Hz, bands 20..63) must be heavily attenuated (< -40 dB)
    for (size_t b = 20; b < 64; ++b) {
        ASSERT_LT(bands[b], 0.01f);
    }
}

// ==============================================================================
// 3. 1 kHz Sine Wave Accuracy Test
// ==============================================================================
TEST_CASE("fft_analyzer_sine_1khz_accuracy") {
    constexpr uint32_t sampleRate = 48000;
    FftAnalyzer analyzer(sampleRate, 64);

    // Exact bin frequency closest to 1000 Hz:
    // bin 21: 21 * 48000 / 1024 = 984.375 Hz
    // bin 22: 22 * 48000 / 1024 = 1031.25 Hz
    // Target 1000 Hz: k = 1000 * 1024 / 48000 = 21.333
    constexpr float freq1k = 1000.0f;

    std::array<float, FftAnalyzer::kFftSize> sineSignal{};
    for (size_t n = 0; n < FftAnalyzer::kFftSize; ++n) {
        double t = static_cast<double>(n) / static_cast<double>(sampleRate);
        sineSignal[n] = static_cast<float>(std::sin(2.0 * M_PI * freq1k * t));
    }

    std::array<float, 64> bands{};
    analyzer.processDirect(sineSignal.data(), bands.data(), 64);

    const auto& magnitudes = analyzer.getLatestMagnitudes();

    // Find the peak bin across the entire spectrum
    size_t peakBin = 0;
    float peakMag = 0.0f;
    for (size_t k = 0; k < FftAnalyzer::kSpectrumBins; ++k) {
        if (magnitudes[k] > peakMag) {
            peakMag = magnitudes[k];
            peakBin = k;
        }
    }

    // Peak MUST be at bin 21 (or adjacent bin 22)
    ASSERT_TRUE(peakBin == 21 || peakBin == 22);
    ASSERT_GE(peakMag, 0.85f); // Due to Hann window spectral leakage across adjacent bins

    // Find the peak display band across 64 logarithmic bands
    size_t peakBand = 0;
    float peakBandVal = 0.0f;
    for (size_t b = 0; b < 64; ++b) {
        if (bands[b] > peakBandVal) {
            peakBandVal = bands[b];
            peakBand = b;
        }
    }

    const auto& bandInfo = analyzer.getBandInfo();
    ASSERT_GE(peakBandVal, 0.85f);
    // The peak band must cover 1000 Hz
    ASSERT_LE(bandInfo[peakBand].minFrequency, freq1k * 1.1f);
    ASSERT_GE(bandInfo[peakBand].maxFrequency, freq1k * 0.9f);

    // Test exact integer bin frequency: 21 * 48000 / 1024 = 984.375 Hz
    constexpr float exactFreq = 21.0f * 48000.0f / 1024.0f;
    for (size_t n = 0; n < FftAnalyzer::kFftSize; ++n) {
        double t = static_cast<double>(n) / static_cast<double>(sampleRate);
        sineSignal[n] = static_cast<float>(std::sin(2.0 * M_PI * exactFreq * t));
    }
    analyzer.processDirect(sineSignal.data(), bands.data(), 64);
    const auto& exactMags = analyzer.getLatestMagnitudes();

    ASSERT_EQ(exactMags[21] >= exactMags[20], true);
    ASSERT_EQ(exactMags[21] >= exactMags[22], true);
    ASSERT_NEAR(exactMags[21], 1.0f, 0.02f);
}

// ==============================================================================
// 4. 10 kHz Sine Wave Accuracy Test
// ==============================================================================
TEST_CASE("fft_analyzer_sine_10khz_accuracy") {
    constexpr uint32_t sampleRate = 48000;
    FftAnalyzer analyzer(sampleRate, 64);

    // Target 10000 Hz: k = 10000 * 1024 / 48000 = 213.333
    constexpr float freq10k = 10000.0f;

    std::array<float, FftAnalyzer::kFftSize> sineSignal{};
    for (size_t n = 0; n < FftAnalyzer::kFftSize; ++n) {
        double t = static_cast<double>(n) / static_cast<double>(sampleRate);
        sineSignal[n] = static_cast<float>(std::sin(2.0 * M_PI * freq10k * t));
    }

    std::array<float, 64> bands{};
    analyzer.processDirect(sineSignal.data(), bands.data(), 64);

    const auto& magnitudes = analyzer.getLatestMagnitudes();

    size_t peakBin = 0;
    float peakMag = 0.0f;
    for (size_t k = 0; k < FftAnalyzer::kSpectrumBins; ++k) {
        if (magnitudes[k] > peakMag) {
            peakMag = magnitudes[k];
            peakBin = k;
        }
    }

    // Peak MUST be at bin 213 (or 214)
    ASSERT_TRUE(peakBin == 213 || peakBin == 214);
    ASSERT_GE(peakMag, 0.85f);

    // In 64 bands, peak band must contain 10 kHz
    size_t peakBand = 0;
    float peakBandVal = 0.0f;
    for (size_t b = 0; b < 64; ++b) {
        if (bands[b] > peakBandVal) {
            peakBandVal = bands[b];
            peakBand = b;
        }
    }

    const auto& bandInfo = analyzer.getBandInfo();
    ASSERT_GE(peakBandVal, 0.85f);
    ASSERT_LE(bandInfo[peakBand].minFrequency, freq10k * 1.1f);
    ASSERT_GE(bandInfo[peakBand].maxFrequency, freq10k * 0.9f);
}

// ==============================================================================
// 5. High-Treble (21 kHz) and Nyquist (24 kHz) Accuracy Test
// ==============================================================================
TEST_CASE("fft_analyzer_high_treble_and_nyquist_accuracy") {
    constexpr uint32_t sampleRate = 48000;
    FftAnalyzer analyzer(sampleRate, 64);

    // 1. Test 21 kHz tone in the top display band (which covers up to 22 kHz)
    constexpr float freq21k = 21000.0f;
    std::array<float, FftAnalyzer::kFftSize> sine21k{};
    for (size_t n = 0; n < FftAnalyzer::kFftSize; ++n) {
        double t = static_cast<double>(n) / static_cast<double>(sampleRate);
        sine21k[n] = static_cast<float>(std::sin(2.0 * M_PI * freq21k * t));
    }

    std::array<float, 64> bands21k{};
    analyzer.processDirect(sine21k.data(), bands21k.data(), 64);

    // The highest display band (band 63, covering ~19.4 kHz to 22.0 kHz) must capture 21 kHz
    ASSERT_GE(bands21k[63], 0.80f);
    // Low frequency bands (<10 kHz, bands 0..40) must be clean (< -40 dB)
    for (size_t b = 0; b < 40; ++b) {
        ASSERT_LT(bands21k[b], 0.01f);
    }

    // 2. Discrete Nyquist cosine: cos(pi * n) = [1, -1, 1, -1, ...]
    std::array<float, FftAnalyzer::kFftSize> nyquistSignal{};
    for (size_t n = 0; n < FftAnalyzer::kFftSize; ++n) {
        nyquistSignal[n] = (n % 2 == 0) ? 1.0f : -1.0f;
    }

    // Unwindowed raw FFT: exact energy isolation at Nyquist bin (kHalfFft = 512)
    std::array<Complex, FftAnalyzer::kFftSize> rawSpectrum{};
    analyzer.computeFftRaw(nyquistSignal.data(), rawSpectrum.data(), false);

    ASSERT_NEAR(rawSpectrum[FftAnalyzer::kHalfFft].magnitude() / 1024.0f, 1.0f, 1e-4f);
    for (size_t k = 0; k < FftAnalyzer::kHalfFft; ++k) {
        ASSERT_NEAR(rawSpectrum[k].magnitude(), 0.0f, 1e-4f);
    }

    // Windowed Nyquist bin is normalized to 1.0
    analyzer.processDirect(nyquistSignal.data(), bands21k.data(), 64);
    const auto& magnitudes = analyzer.getLatestMagnitudes();
    ASSERT_NEAR(magnitudes[FftAnalyzer::kHalfFft], 1.0f, 0.02f);
}

// ==============================================================================
// 6. Logarithmic Display Bands Properties Test (8 Hz to 22 kHz)
// ==============================================================================
TEST_CASE("fft_analyzer_logarithmic_bands_properties") {
    // Test both 32-band and 64-band configurations
    for (size_t numBands : {32U, 64U}) {
        FftAnalyzer analyzer(48000, numBands);
        const auto& bandInfo = analyzer.getBandInfo();

        float prevCenter = 0.0f;
        for (size_t b = 0; b < numBands; ++b) {
            const auto& info = bandInfo[b];

            // Strict monotonic increase in center frequencies
            ASSERT_GT(info.centerFrequency, prevCenter);
            prevCenter = info.centerFrequency;

            // Frequencies within calibrated audio spectrum (8 Hz to 22 kHz)
            ASSERT_GE(info.minFrequency, 7.9f);
            ASSERT_LE(info.maxFrequency, 22001.0f);
            ASSERT_LE(info.minFrequency, info.centerFrequency);
            ASSERT_LE(info.centerFrequency, info.maxFrequency);

            // FFT bin index validity
            ASSERT_LE(info.startBin, info.endBin);
            ASSERT_LE(info.endBin, static_cast<uint32_t>(FftAnalyzer::kHalfFft));
        }

        // Verify highest band reaches 22 kHz
        ASSERT_GE(bandInfo[numBands - 1].maxFrequency, 21990.0f);
    }
}

// ==============================================================================
// 7. Lock-Free Thread Safety and Streaming Push Test
// ==============================================================================
TEST_CASE("fft_analyzer_lockfree_thread_safety") {
    constexpr uint32_t sampleRate = 48000;
    FftAnalyzer analyzer(sampleRate, 64);

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> readSuccessCount{0};
    std::atomic<uint64_t> totalReads{0};

    constexpr size_t totalPushFrames = 150000;
    constexpr size_t chunkFrames = 192; // Typical Oboe audio burst size

    // Producer Thread: Simulates real-time audio playback callback pushing samples
    std::thread producerThread([&]() {
        std::vector<float> audioChunk(chunkFrames * 2); // Stereo interleaved
        size_t framesPushed = 0;
        float phase = 0.0f;

        while (framesPushed < totalPushFrames) {
            // Generate dynamic test tone
            for (size_t i = 0; i < chunkFrames; ++i) {
                float sample = std::sin(phase);
                audioChunk[i * 2] = sample;
                audioChunk[i * 2 + 1] = sample;
                phase += 0.13f;
                if (phase > 2.0f * static_cast<float>(M_PI)) {
                    phase -= 2.0f * static_cast<float>(M_PI);
                }
            }

            analyzer.pushStereo(audioChunk.data(), chunkFrames);
            framesPushed += chunkFrames;

            // Small yield to interleave execution with consumer
            std::this_thread::yield();
        }

        producerDone.store(true, std::memory_order_release);
    });

    // Consumer Thread 1: Simulates 60 FPS JNI UI Polling
    std::thread consumerThread1([&]() {
        std::array<float, 64> snapshot{};
        while (!producerDone.load(std::memory_order_acquire)) {
            bool fresh = analyzer.getLatestBands(snapshot.data(), 64);
            totalReads.fetch_add(1, std::memory_order_relaxed);
            if (fresh) {
                readSuccessCount.fetch_add(1, std::memory_order_relaxed);
                // Verify all values are valid finite numbers without tearing or NaN
                for (float val : snapshot) {
                    ASSERT_TRUE(std::isfinite(val));
                    ASSERT_GE(val, 0.0f);
                    ASSERT_LE(val, 2.0f);
                }
            }
            std::this_thread::yield();
        }
    });

    // Consumer Thread 2: Concurrent secondary consumer
    std::thread consumerThread2([&]() {
        std::array<float, 64> snapshot{};
        while (!producerDone.load(std::memory_order_acquire)) {
            analyzer.getLatestBands(snapshot.data(), 64);
            std::this_thread::yield();
        }
    });

    producerThread.join();
    consumerThread1.join();
    consumerThread2.join();

    // Verify snapshots were extracted successfully
    ASSERT_GT(readSuccessCount.load(), 10ULL);
    ASSERT_GT(totalReads.load(), 50ULL);
}
