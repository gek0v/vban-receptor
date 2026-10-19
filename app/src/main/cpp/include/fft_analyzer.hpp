#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <array>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <concepts>
#include <span>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vban {

/**
 * @brief High-performance lightweight complex number representation.
 *
 * Trivially copyable, cache-friendly, and optimized for auto-vectorization
 * and inlined butterfly execution.
 */
struct alignas(8) Complex {
    float real{0.0f};
    float imag{0.0f};

    constexpr Complex() noexcept = default;
    constexpr Complex(float r, float i) noexcept : real(r), imag(i) {}

    constexpr Complex operator+(const Complex& o) const noexcept {
        return {real + o.real, imag + o.imag};
    }

    constexpr Complex operator-(const Complex& o) const noexcept {
        return {real - o.real, imag - o.imag};
    }

    constexpr Complex operator*(const Complex& o) const noexcept {
        return {real * o.real - imag * o.imag, real * o.imag + imag * o.real};
    }

    [[nodiscard]] float magnitude() const noexcept {
        return std::sqrt(real * real + imag * imag);
    }

    [[nodiscard]] float magnitudeSquared() const noexcept {
        return real * real + imag * imag;
    }
};

/**
 * @brief Frequency and bin boundary metadata for a logarithmic display band.
 */
struct BandInfo {
    float centerFrequency{0.0f};
    float minFrequency{0.0f};
    float maxFrequency{0.0f};
    uint32_t startBin{0};
    uint32_t endBin{0};
};

/**
 * @brief Wait-free, lock-free Triple Buffer for atomic spectrum snapshot exchange.
 *
 * Allows an audio thread (single producer) to continuously publish fresh spectrum
 * bands while a UI/JNI thread (single consumer) queries snapshots at 60 FPS without
 * blocking, spin-waiting, or memory allocations.
 */
template <typename T, size_t Size>
class alignas(64) FftTripleBuffer {
public:
    FftTripleBuffer() noexcept {
        for (auto& buf : m_storage) {
            buf.fill(T{});
        }
    }

    // Single producer (audio thread) writes new band snapshot
    void write(const T* src, size_t count) noexcept {
        const size_t toCopy = std::min(Size, count);
        std::memcpy(m_storage[m_writeSlot].data(), src, toCopy * sizeof(T));
        if (toCopy < Size) {
            std::memset(m_storage[m_writeSlot].data() + toCopy, 0, (Size - toCopy) * sizeof(T));
        }

        // Atomically publish writeSlot to sharedState:
        // bit 0-1: slot index (0..2)
        // bit 2: has_new_data flag (1)
        // bit 3-31: monotonic generation counter
        uint32_t currentShared = m_sharedState.load(std::memory_order_relaxed);
        uint32_t newShared;
        do {
            uint32_t gen = (currentShared >> 3) + 1;
            newShared = (gen << 3) | 4U | static_cast<uint32_t>(m_writeSlot);
        } while (!m_sharedState.compare_exchange_weak(
            currentShared, newShared,
            std::memory_order_release,
            std::memory_order_relaxed));

        // Reclaim previous shared slot for next write
        m_writeSlot = static_cast<uint8_t>(currentShared & 3U);
    }

    // Single consumer (UI / JNI thread) reads latest snapshot without blocking
    bool read(T* dst, size_t count) noexcept {
        uint32_t expected = m_sharedState.load(std::memory_order_acquire);
        bool hasNew = (expected & 4U) != 0;

        if (hasNew) {
            // Swap consumer's current readSlot into sharedState, clearing the new data flag
            uint32_t desired;
            do {
                uint32_t gen = expected >> 3;
                desired = (gen << 3) | static_cast<uint32_t>(m_readSlot); // bit 2 is 0
            } while (!m_sharedState.compare_exchange_weak(
                expected, desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire));

            m_readSlot = static_cast<uint8_t>(expected & 3U);
        }

        const size_t toCopy = std::min(Size, count);
        std::memcpy(dst, m_storage[m_readSlot].data(), toCopy * sizeof(T));
        return hasNew;
    }

private:
    std::array<std::array<T, Size>, 3> m_storage{};
    alignas(64) std::atomic<uint32_t> m_sharedState{0};
    uint8_t m_writeSlot{1};
    uint8_t m_readSlot{0};
};

/**
 * @brief Real-Time 1024-Point FFT Spectral Analyzer.
 *
 * Engineered for zero-allocation, lock-free real-time audio visualization:
 * - 1024-point Radix-2 Cooley-Tukey DIT FFT with precomputed bit-reversal and twiddle tables.
 * - Perfectly symmetric Hann window precomputed with coherent gain normalization.
 * - Logarithmic display bands (supports 32 or 64 bands) covering 8 Hz to 22 kHz.
 * - Lock-free, wait-free Triple Buffer snapshot extraction for 60 FPS JNI/Compose UI visualizers.
 * - Audio callback safe: zero allocations, zero mutexes, zero syscalls.
 */
class FftAnalyzer {
public:
    static constexpr size_t kFftSize = 1024;
    static constexpr size_t kHalfFft = kFftSize / 2;       // 512 bins
    static constexpr size_t kSpectrumBins = kHalfFft + 1;   // 513 unique frequency bins [0..512]
    static constexpr size_t kMaxBands = 64;
    static constexpr size_t kDefaultHopSize = 512;          // 50% overlap for fluid 60 FPS updates
    static constexpr size_t kHistorySize = 2048;            // Circular sample accumulation buffer

    explicit FftAnalyzer(uint32_t sampleRate = 48000, size_t numBands = 64) noexcept;
    ~FftAnalyzer() = default;

    // Non-copyable and non-movable to guarantee thread and address safety
    FftAnalyzer(const FftAnalyzer&) = delete;
    FftAnalyzer& operator=(const FftAnalyzer&) = delete;
    FftAnalyzer(FftAnalyzer&&) = delete;
    FftAnalyzer& operator=(FftAnalyzer&&) = delete;

    /**
     * @brief Configure sample rate and logarithmic band count.
     * Note: Invoke before launching audio threads or during stream re-configuration.
     */
    void configure(uint32_t sampleRate, size_t numBands = 64) noexcept;

    /**
     * @brief Pushes mono audio samples from audio thread into the circular accumulator.
     * When hop size is reached, executes windowed FFT and updates the snapshot buffer.
     * Zero allocations, wait-free, audio callback safe.
     *
     * @param samples Pointer to float audio samples [-1.0f, 1.0f].
     * @param count Number of samples to push.
     * @param stride Stride between samples (e.g. 1 for mono, 2 for interleaved stereo).
     */
    void pushSamples(const float* samples, size_t count, size_t stride = 1) noexcept;

    /**
     * @brief Pushes interleaved stereo audio samples, mixing (L + R) * 0.5f to mono.
     * Zero allocations, wait-free, audio callback safe.
     *
     * @param interleaved Pointer to interleaved [L, R, L, R, ...] float audio frames.
     * @param frames Number of stereo frames to push.
     */
    void pushStereo(const float* interleaved, size_t frames) noexcept;

    /**
     * @brief Synchronous direct execution on an exact 1024-sample buffer.
     * Designed for testing, offline benchmarking, and unit test verification.
     *
     * @param input1024 Exactly 1024 input samples.
     * @param outBands Array to receive logarithmic display bands.
     * @param numBands Number of bands to extract (e.g. 32 or 64).
     */
    void processDirect(const float* input1024, float* outBands, size_t numBands) noexcept;

    /**
     * @brief Executes raw 1024-point FFT into a complex output buffer.
     *
     * @param input1024 1024 real input samples.
     * @param outSpectrum1024 1024 complex bins.
     * @param applyWindow If true, applies precomputed Hann window; if false, rectangular.
     */
    void computeFftRaw(const float* input1024, Complex* outSpectrum1024, bool applyWindow = true) noexcept;

    /**
     * @brief Wait-free, lock-free snapshot extraction for UI/JNI threads.
     *
     * @param outBands Destination array receiving normalized magnitudes [0.0f, 1.0f].
     * @param numBands Number of bands to copy (typically 32 or 64).
     * @return true if fresh snapshot was available, false if unchanged.
     */
    bool getLatestBands(float* outBands, size_t numBands) noexcept;

    /**
     * @brief Reset internal sample accumulators and snapshot buffers to silence.
     */
    void reset() noexcept;

    // Const Accessors for verification and telemetry
    [[nodiscard]] uint32_t getSampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] size_t getNumBands() const noexcept { return m_numBands; }
    [[nodiscard]] float getWindowSum() const noexcept { return m_windowSum; }
    [[nodiscard]] const std::array<float, kFftSize>& getWindow() const noexcept { return m_window; }
    [[nodiscard]] const std::array<BandInfo, kMaxBands>& getBandInfo() const noexcept { return m_bandInfo; }
    [[nodiscard]] const std::array<float, kSpectrumBins>& getLatestMagnitudes() const noexcept { return m_magnitudes; }

    [[nodiscard]] float getBinFrequency(size_t binIndex) const noexcept {
        return static_cast<float>(binIndex) * static_cast<float>(m_sampleRate) / static_cast<float>(kFftSize);
    }

private:
    void initTables() noexcept;
    void setupLogBands() noexcept;
    void executeFftAndPublish() noexcept;

    uint32_t m_sampleRate{48000};
    size_t m_numBands{64};
    size_t m_hopSize{kDefaultHopSize};
    float m_windowSum{512.0f};

    // Precomputed lookup tables (zero-allocation during runtime)
    std::array<uint16_t, kFftSize> m_bitReverseTable{};
    std::array<float, kFftSize> m_window{};
    std::array<Complex, kHalfFft> m_twiddles{};
    std::array<BandInfo, kMaxBands> m_bandInfo{};

    // Audio stream circular accumulator
    std::array<float, kHistorySize> m_sampleHistory{};
    size_t m_historyWritePos{0};
    size_t m_samplesSinceLastFft{0};

    // Reusable scratch buffers for FFT execution (zero heap allocation)
    alignas(16) std::array<float, kFftSize> m_fftInputScratch{};
    alignas(16) std::array<Complex, kFftSize> m_fftOutputScratch{};
    alignas(16) std::array<float, kSpectrumBins> m_magnitudes{};
    alignas(16) std::array<float, kMaxBands> m_bandScratch{};

    // Lock-free Triple Buffer for atomic snapshot exchange with UI
    FftTripleBuffer<float, kMaxBands> m_tripleBuffer;
};

} // namespace vban
