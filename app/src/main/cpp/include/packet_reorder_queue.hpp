#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>
#include <cmath>
#include <chrono>
#include <algorithm>

namespace vban {

/**
 * @brief Lock-free, zero-heap-allocation UDP Packet Reordering Queue & Single-Packet Loss Concealment (PLC).
 *
 * Implements a fixed 16-slot circular reorder window indexed by sequence number (nuFrame % SlotCount).
 * Reassembles out-of-order UDP packets arriving over Wi-Fi without audio glitches.
 * If a 1-packet gap occurs (e.g. 101, then 103 and 102 never arrives after slot expiration),
 * extrapolates 1 packet with smooth raised-cosine decay from the previous frame tail.
 * If a gap > 1 packet occurs, flushes the queue and resets sequence tracking.
 *
 * @tparam SampleType Data type of audio samples (default: float)
 * @tparam SlotCount Capacity of circular reorder queue in packets (default: 16)
 * @tparam MaxSamplesPerPacket Maximum audio samples per incoming packet (default: 4096)
 */
template <typename SampleType = float, size_t SlotCount = 16, size_t MaxSamplesPerPacket = 4096>
class PacketReorderQueue {
public:
    static_assert(SlotCount > 0 && (SlotCount & (SlotCount - 1)) == 0, "SlotCount must be a power of two");

    PacketReorderQueue() noexcept {
        reset();
    }

    ~PacketReorderQueue() = default;

    // Non-copyable, movable
    PacketReorderQueue(const PacketReorderQueue&) = delete;
    PacketReorderQueue& operator=(const PacketReorderQueue&) = delete;
    PacketReorderQueue(PacketReorderQueue&&) noexcept = default;
    PacketReorderQueue& operator=(PacketReorderQueue&&) noexcept = default;

    /**
     * @brief Reset all queue slots, sequence tracking, and PLC history.
     */
    void reset() noexcept {
        m_initialized = false;
        m_expectedNuFrame = 0;
        m_highestNuFrame = 0;
        m_lastValidSampleCount = 0;
        m_consecutivePlcCount = 0;
        m_gapDetected = false;
        m_gapMissingNuFrame = 0;

        for (auto& slot : m_slots) {
            slot.valid = false;
            slot.sampleCount = 0;
            slot.nuFrame = 0;
        }
    }

    /**
     * @brief Push an incoming audio packet into the circular reorder queue.
     *
     * @param nuFrame 32-bit unsigned VBAN sequence number.
     * @param samples Pointer to interleaved Float32 audio samples.
     * @param sampleCount Total number of samples (samplesPerFrame * channels).
     * @return true if packet was accepted into a slot, false if rejected (duplicate, late, invalid).
     */
    bool push(uint32_t nuFrame, const SampleType* samples, size_t sampleCount) noexcept {
        if (!samples || sampleCount == 0 || sampleCount > MaxSamplesPerPacket) [[unlikely]] {
            return false;
        }

        if (!m_initialized) {
            m_initialized = true;
            m_expectedNuFrame = nuFrame;
            m_highestNuFrame = nuFrame;
            m_consecutivePlcCount = 0;
            m_gapDetected = false;

            const size_t slotIdx = nuFrame % SlotCount;
            m_slots[slotIdx].nuFrame = nuFrame;
            m_slots[slotIdx].sampleCount = static_cast<uint32_t>(sampleCount);
            m_slots[slotIdx].valid = true;
            std::memcpy(m_slots[slotIdx].samples.data(), samples, sampleCount * sizeof(SampleType));
            return true;
        }

        // Modular sequence difference against current expected sequence number
        const uint32_t delta = nuFrame - m_expectedNuFrame;

        // Late or duplicate packet (modular delta >= 0x80000000U means nuFrame < expectedNuFrame)
        if (delta >= 0x80000000U) {
            return false;
        }

        // Sequence jump exceeding circular buffer capacity: flush and reset tracking
        if (delta >= SlotCount) {
            flushAndResetAll(nuFrame);
            const size_t slotIdx = nuFrame % SlotCount;
            m_slots[slotIdx].nuFrame = nuFrame;
            m_slots[slotIdx].sampleCount = static_cast<uint32_t>(sampleCount);
            m_slots[slotIdx].valid = true;
            std::memcpy(m_slots[slotIdx].samples.data(), samples, sampleCount * sizeof(SampleType));
            return true;
        }

        // Modular sequence difference against highest sequence number seen
        const uint32_t jump = nuFrame - m_highestNuFrame;
        if (jump > 0 && jump < 0x80000000U) {
            // Forward jump:
            // jump == 1: consecutive packet (0 skipped)
            // jump == 2: single-packet gap (1 skipped, e.g. 101 -> 103, skipping 102)
            // jump > 2: gap > 1 packet -> flush queue and reset sequence tracking
            if (jump > 2) {
                flushAndResetAll(nuFrame);
                const size_t slotIdx = nuFrame % SlotCount;
                m_slots[slotIdx].nuFrame = nuFrame;
                m_slots[slotIdx].sampleCount = static_cast<uint32_t>(sampleCount);
                m_slots[slotIdx].valid = true;
                std::memcpy(m_slots[slotIdx].samples.data(), samples, sampleCount * sizeof(SampleType));
                return true;
            }
            m_highestNuFrame = nuFrame;
        }

        // Normal slot insertion
        const size_t slotIdx = nuFrame % SlotCount;
        m_slots[slotIdx].nuFrame = nuFrame;
        m_slots[slotIdx].sampleCount = static_cast<uint32_t>(sampleCount);
        m_slots[slotIdx].valid = true;
        std::memcpy(m_slots[slotIdx].samples.data(), samples, sampleCount * sizeof(SampleType));
        return true;
    }

    /**
     * @brief Pop all contiguous in-order samples from the queue.
     *
     * If a single-packet gap is detected and the missing slot has expired,
     * performs PLC extrapolation with smooth raised-cosine decay from previous frame tail.
     * If a gap > 1 packet occurs, flushes the queue and resets sequence tracking.
     *
     * @param outSamples Destination buffer for audio samples.
     * @param maxSamples Capacity of destination buffer in samples.
     * @param wasPlc Output flag set to true if any popped packet was extrapolated via PLC.
     * @return Number of samples written into outSamples.
     */
    size_t popOrdered(SampleType* outSamples, size_t maxSamples, bool& wasPlc) noexcept {
        wasPlc = false;
        if (!m_initialized || !outSamples || maxSamples == 0) [[unlikely]] {
            return 0;
        }

        size_t totalPopped = 0;

        while (totalPopped < maxSamples) {
            const size_t slotIdx = m_expectedNuFrame % SlotCount;

            // 1. Expected packet is present and valid
            if (m_slots[slotIdx].valid && m_slots[slotIdx].nuFrame == m_expectedNuFrame) {
                const size_t count = m_slots[slotIdx].sampleCount;
                if (count > (maxSamples - totalPopped)) {
                    // Not enough space for complete packet in destination buffer
                    break;
                }

                std::memcpy(outSamples + totalPopped, m_slots[slotIdx].samples.data(), count * sizeof(SampleType));

                // Cache valid samples for potential future PLC extrapolation
                std::memcpy(m_lastValidSamples.data(), m_slots[slotIdx].samples.data(), count * sizeof(SampleType));
                m_lastValidSampleCount = count;

                totalPopped += count;
                m_slots[slotIdx].valid = false;
                m_expectedNuFrame++;
                m_consecutivePlcCount = 0;
                m_gapDetected = false;
                continue;
            }

            // 2. Expected packet is NOT in its slot.
            // Check if there are any future packets in the queue.
            const uint32_t forwardDistance = m_highestNuFrame - m_expectedNuFrame;
            if (forwardDistance >= 0x80000000U || forwardDistance == 0) {
                // Queue holds no future packets; waiting for next consecutive packet
                break;
            }

            // Find the next available packet sequence number
            uint32_t nextAvailableNuFrame = 0;
            bool foundNext = false;
            for (size_t offset = 1; offset < SlotCount; ++offset) {
                const uint32_t candidateNu = m_expectedNuFrame + static_cast<uint32_t>(offset);
                const size_t candidateIdx = candidateNu % SlotCount;
                if (m_slots[candidateIdx].valid && m_slots[candidateIdx].nuFrame == candidateNu) {
                    nextAvailableNuFrame = candidateNu;
                    foundNext = true;
                    break;
                }
            }

            if (!foundNext) {
                break;
            }

            // Check slot expiration condition
            bool slotExpired = false;
            if (forwardDistance >= m_reorderDepth) {
                slotExpired = true;
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (!m_gapDetected || m_gapMissingNuFrame != m_expectedNuFrame) {
                    m_gapDetected = true;
                    m_gapMissingNuFrame = m_expectedNuFrame;
                    m_gapDetectedTime = now;
                }
                if (m_timeoutMs <= 0.0) {
                    slotExpired = true;
                } else {
                    const double elapsedMs = std::chrono::duration<double, std::milli>(now - m_gapDetectedTime).count();
                    if (elapsedMs >= m_timeoutMs) {
                        slotExpired = true;
                    }
                }
            }

            if (!slotExpired) {
                // Slot has not expired yet; hold off to give out-of-order packet time to arrive
                break;
            }

            // Slot has expired!
            const uint32_t missingCount = nextAvailableNuFrame - m_expectedNuFrame;
            if (missingCount > 1 || m_consecutivePlcCount >= 1) {
                // Gap > 1 packet: flush queue and reset sequence tracking to next available packet
                flushAndResetTo(nextAvailableNuFrame);
                continue;
            }

            // Single-packet gap (missingCount == 1 and m_consecutivePlcCount == 0):
            // Extrapolate 1 packet with smooth raised-cosine decay from previous frame tail.
            const size_t nextSlotIdx = nextAvailableNuFrame % SlotCount;
            size_t plcCount = m_slots[nextSlotIdx].sampleCount;
            if (plcCount == 0 && m_lastValidSampleCount > 0) {
                plcCount = m_lastValidSampleCount;
            }
            if (plcCount == 0) {
                plcCount = 256;
            }

            if (plcCount > (maxSamples - totalPopped)) {
                break;
            }

            constexpr float PI = 3.14159265358979323846f;
            for (size_t i = 0; i < plcCount; ++i) {
                const float progress = static_cast<float>(i) / static_cast<float>(plcCount);
                // Raised cosine decay: 0.5 * (1.0 + cos(pi * progress))
                // Starts at 1.0 (continuous with last frame tail) and decays smoothly to 0.0
                const float decay = 0.5f * (1.0f + std::cos(PI * progress));
                const SampleType base = (m_lastValidSampleCount > 0)
                    ? m_lastValidSamples[i % m_lastValidSampleCount]
                    : SampleType{0};
                outSamples[totalPopped + i] = static_cast<SampleType>(base * decay);
            }

            // Cache generated PLC samples
            std::memcpy(m_lastValidSamples.data(), outSamples + totalPopped, plcCount * sizeof(SampleType));
            m_lastValidSampleCount = plcCount;

            totalPopped += plcCount;
            m_expectedNuFrame++;
            m_consecutivePlcCount = 1;
            wasPlc = true;
            m_gapDetected = false;
        }

        return totalPopped;
    }

    /**
     * @brief Configure packet reorder depth (number of slots ahead before expiring a missing packet).
     */
    void setReorderDepth(size_t depth) noexcept {
        m_reorderDepth = std::max<size_t>(1, depth);
    }

    /**
     * @brief Configure slot expiration timeout in milliseconds.
     */
    void setTimeoutMs(double timeoutMs) noexcept {
        m_timeoutMs = timeoutMs;
    }

    [[nodiscard]] uint32_t getExpectedNuFrame() const noexcept {
        return m_expectedNuFrame;
    }

    [[nodiscard]] bool isInitialized() const noexcept {
        return m_initialized;
    }

    [[nodiscard]] size_t getQueuedCount() const noexcept {
        size_t count = 0;
        for (const auto& slot : m_slots) {
            if (slot.valid) {
                count++;
            }
        }
        return count;
    }

private:
    struct Slot {
        uint32_t nuFrame{0};
        uint32_t sampleCount{0};
        bool valid{false};
        alignas(16) std::array<SampleType, MaxSamplesPerPacket> samples{};
    };

    void flushAndResetAll(uint32_t newNuFrame) noexcept {
        for (auto& slot : m_slots) {
            slot.valid = false;
            slot.sampleCount = 0;
            slot.nuFrame = 0;
        }
        m_expectedNuFrame = newNuFrame;
        m_highestNuFrame = newNuFrame;
        m_consecutivePlcCount = 0;
        m_gapDetected = false;
    }

    void flushAndResetTo(uint32_t newNuFrame) noexcept {
        for (size_t i = 0; i < SlotCount; ++i) {
            if (m_slots[i].valid) {
                const uint32_t d = m_slots[i].nuFrame - newNuFrame;
                if (d >= 0x80000000U || m_slots[i].nuFrame < newNuFrame) {
                    m_slots[i].valid = false;
                }
            }
        }
        m_expectedNuFrame = newNuFrame;
        if (m_highestNuFrame - newNuFrame >= 0x80000000U || m_highestNuFrame < newNuFrame) {
            m_highestNuFrame = newNuFrame;
        }
        m_consecutivePlcCount = 0;
        m_gapDetected = false;
    }

    alignas(64) std::array<Slot, SlotCount> m_slots{};
    alignas(16) std::array<SampleType, MaxSamplesPerPacket> m_lastValidSamples{};
    size_t m_lastValidSampleCount{0};

    uint32_t m_expectedNuFrame{0};
    uint32_t m_highestNuFrame{0};
    bool m_initialized{false};

    size_t m_reorderDepth{2};
    double m_timeoutMs{10.0};

    std::chrono::steady_clock::time_point m_gapDetectedTime{};
    bool m_gapDetected{false};
    uint32_t m_gapMissingNuFrame{0};
    size_t m_consecutivePlcCount{0};
};

} // namespace vban
