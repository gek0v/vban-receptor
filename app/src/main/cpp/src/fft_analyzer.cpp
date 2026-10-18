#include "fft_analyzer.hpp"
#include <numeric>

namespace vban {

FftAnalyzer::FftAnalyzer(uint32_t sampleRate, size_t numBands) noexcept
    : m_sampleRate(sampleRate > 0 ? sampleRate : 48000),
      m_numBands(std::clamp<size_t>(numBands, 16, kMaxBands)) {
    initTables();
    setupLogBands();
    reset();
}

void FftAnalyzer::configure(uint32_t sampleRate, size_t numBands) noexcept {
    if (sampleRate > 0) {
        m_sampleRate = sampleRate;
    }
    m_numBands = std::clamp<size_t>(numBands, 16, kMaxBands);
    setupLogBands();
    reset();
}

void FftAnalyzer::reset() noexcept {
    m_historyWritePos = 0;
    m_samplesSinceLastFft = 0;
    m_sampleHistory.fill(0.0f);
    m_fftInputScratch.fill(0.0f);
    for (auto& c : m_fftOutputScratch) {
        c = Complex{0.0f, 0.0f};
    }
    m_magnitudes.fill(0.0f);
    m_bandScratch.fill(0.0f);
}

void FftAnalyzer::initTables() noexcept {
    // 1. Precompute 10-bit bit-reversal permutation table for 1024 points
    for (size_t i = 0; i < kFftSize; ++i) {
        uint16_t rev = 0;
        uint16_t val = static_cast<uint16_t>(i);
        for (int b = 0; b < 10; ++b) {
            if ((val >> b) & 1) {
                rev |= static_cast<uint16_t>(1 << (9 - b));
            }
        }
        m_bitReverseTable[i] = rev;
    }

    // 2. Precompute perfectly symmetric Hann window: w[n] = 0.5 * (1 - cos(2*pi*n / (N - 1)))
    double sum = 0.0;
    const double denom = static_cast<double>(kFftSize - 1);
    for (size_t i = 0; i < kFftSize; ++i) {
        double w = 0.5 * (1.0 - std::cos((2.0 * M_PI * static_cast<double>(i)) / denom));
        m_window[i] = static_cast<float>(w);
        sum += w;
    }
    m_windowSum = static_cast<float>(sum);

    // 3. Precompute twiddle factors W_N^k = e^(-i * 2*pi*k / N) for k in [0, 511]
    const double fftSizeD = static_cast<double>(kFftSize);
    for (size_t i = 0; i < kHalfFft; ++i) {
        double angle = -2.0 * M_PI * static_cast<double>(i) / fftSizeD;
        m_twiddles[i] = Complex{
            static_cast<float>(std::cos(angle)),
            static_cast<float>(std::sin(angle))
        };
    }
}

void FftAnalyzer::setupLogBands() noexcept {
    // Frequency range: 20 Hz to Nyquist (up to 48 kHz max for high-res streams)
    const float fMin = 20.0f;
    float fMax = std::min(48000.0f, static_cast<float>(m_sampleRate) * 0.5f);
    if (fMax <= fMin) {
        fMax = static_cast<float>(m_sampleRate) * 0.5f;
    }

    const double ratio = std::pow(static_cast<double>(fMax) / fMin, 1.0 / static_cast<double>(m_numBands));
    const float binWidth = static_cast<float>(m_sampleRate) / static_cast<float>(kFftSize);

    for (size_t b = 0; b < m_numBands; ++b) {
        float fLow = static_cast<float>(fMin * std::pow(ratio, static_cast<double>(b)));
        float fHigh = static_cast<float>(fMin * std::pow(ratio, static_cast<double>(b + 1)));
        float fCenter = std::sqrt(fLow * fHigh);

        uint32_t startBin = static_cast<uint32_t>(fLow / binWidth);
        uint32_t endBin = static_cast<uint32_t>(std::ceil(fHigh / binWidth));

        if (b == 0 && startBin == 0) {
            // First display band covers from DC / lowest frequency
            startBin = 0;
        } else if (startBin == 0) {
            startBin = 1;
        }

        startBin = std::min<uint32_t>(startBin, static_cast<uint32_t>(kHalfFft));
        endBin = std::clamp<uint32_t>(endBin, startBin, static_cast<uint32_t>(kHalfFft));

        m_bandInfo[b] = BandInfo{
            .centerFrequency = fCenter,
            .minFrequency = fLow,
            .maxFrequency = fHigh,
            .startBin = startBin,
            .endBin = endBin
        };
    }

    // Zero out unused band metadata
    for (size_t b = m_numBands; b < kMaxBands; ++b) {
        m_bandInfo[b] = BandInfo{};
    }
}

void FftAnalyzer::computeFftRaw(const float* input1024, Complex* outSpectrum1024, bool applyWindow) noexcept {
    // 1. Bit-reversal permutation with optional Hann windowing
    for (size_t i = 0; i < kFftSize; ++i) {
        const size_t rev = m_bitReverseTable[i];
        float val = input1024[i];
        if (applyWindow) {
            val *= m_window[i];
        }
        outSpectrum1024[rev] = Complex{val, 0.0f};
    }

    // 2. Cooley-Tukey Radix-2 Butterfly Stages (10 stages for 1024 points)
    for (size_t len = 2; len <= kFftSize; len <<= 1) {
        const size_t halfLen = len >> 1;
        const size_t step = kFftSize / len;

        for (size_t i = 0; i < kFftSize; i += len) {
            for (size_t j = 0; j < halfLen; ++j) {
                const Complex w = m_twiddles[j * step];
                const Complex u = outSpectrum1024[i + j];
                const Complex v = outSpectrum1024[i + j + halfLen] * w;

                outSpectrum1024[i + j] = u + v;
                outSpectrum1024[i + j + halfLen] = u - v;
            }
        }
    }
}

void FftAnalyzer::processDirect(const float* input1024, float* outBands, size_t numBands) noexcept {
    // Compute windowed FFT
    computeFftRaw(input1024, m_fftOutputScratch.data(), true);

    // Compute normalized magnitudes across [0..kHalfFft]
    const float invWindowSum = (m_windowSum > 1e-6f) ? (1.0f / m_windowSum) : (1.0f / 512.0f);
    const float acScale = invWindowSum * 2.0f;

    // DC bin (0 Hz) and Nyquist bin (kHalfFft) have factor 1.0; AC bins have factor 2.0
    m_magnitudes[0] = m_fftOutputScratch[0].magnitude() * invWindowSum;
    for (size_t k = 1; k < kHalfFft; ++k) {
        m_magnitudes[k] = m_fftOutputScratch[k].magnitude() * acScale;
    }
    m_magnitudes[kHalfFft] = m_fftOutputScratch[kHalfFft].magnitude() * invWindowSum;

    // Aggregate logarithmic display bands
    const size_t count = std::min(numBands, m_numBands);
    for (size_t b = 0; b < count; ++b) {
        const auto& info = m_bandInfo[b];
        float maxVal = 0.0f;
        for (uint32_t k = info.startBin; k <= info.endBin; ++k) {
            if (k <= kHalfFft) {
                maxVal = std::max(maxVal, m_magnitudes[k]);
            }
        }
        outBands[b] = maxVal;
    }
    for (size_t b = count; b < numBands; ++b) {
        outBands[b] = 0.0f;
    }
}

void FftAnalyzer::pushSamples(const float* samples, size_t count, size_t stride) noexcept {
    if (!samples || count == 0 || stride == 0) {
        return;
    }

    constexpr size_t mask = kHistorySize - 1;
    for (size_t i = 0; i < count; ++i) {
        m_sampleHistory[m_historyWritePos & mask] = samples[i * stride];
        m_historyWritePos++;
        m_samplesSinceLastFft++;

        if (m_samplesSinceLastFft >= m_hopSize) {
            m_samplesSinceLastFft = 0;
            executeFftAndPublish();
        }
    }
}

void FftAnalyzer::pushStereo(const float* interleaved, size_t frames) noexcept {
    if (!interleaved || frames == 0) {
        return;
    }

    constexpr size_t mask = kHistorySize - 1;
    for (size_t i = 0; i < frames; ++i) {
        // High-fidelity mono downmix: (L + R) * 0.5f
        const float mono = (interleaved[i * 2] + interleaved[i * 2 + 1]) * 0.5f;
        m_sampleHistory[m_historyWritePos & mask] = mono;
        m_historyWritePos++;
        m_samplesSinceLastFft++;

        if (m_samplesSinceLastFft >= m_hopSize) {
            m_samplesSinceLastFft = 0;
            executeFftAndPublish();
        }
    }
}

void FftAnalyzer::executeFftAndPublish() noexcept {
    constexpr size_t mask = kHistorySize - 1;
    const size_t startPos = (m_historyWritePos >= kFftSize) ? (m_historyWritePos - kFftSize) : 0;

    // Linearize the last 1024 contiguous samples
    for (size_t i = 0; i < kFftSize; ++i) {
        m_fftInputScratch[i] = m_sampleHistory[(startPos + i) & mask];
    }

    // Direct process into m_bandScratch
    processDirect(m_fftInputScratch.data(), m_bandScratch.data(), m_numBands);

    // Atomically publish fresh magnitude snapshot to Triple Buffer
    m_tripleBuffer.write(m_bandScratch.data(), m_numBands);
}

bool FftAnalyzer::getLatestBands(float* outBands, size_t numBands) noexcept {
    if (!outBands || numBands == 0) {
        return false;
    }
    return m_tripleBuffer.read(outBands, std::min(numBands, kMaxBands));
}

} // namespace vban
