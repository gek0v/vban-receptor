#include "test_framework.hpp"
#include "spsc_ring_buffer.hpp"

#include <vector>
#include <thread>
#include <chrono>
#include <numeric>
#include <span>

// ==============================================================================
// 1. Compile-Time Static Assertions & Layout Verification
// ==============================================================================
TEST_CASE("spsc_buffer_compile_time_power_of_two_and_alignment") {
    // Valid power-of-two capacities must compile
    using Buf2   = vban::SpscRingBuffer<float, 2>;
    using Buf4   = vban::SpscRingBuffer<float, 4>;
    using Buf16  = vban::SpscRingBuffer<float, 16>;
    using Buf256 = vban::SpscRingBuffer<float, 256>;
    using Buf4k  = vban::SpscRingBuffer<float, 4096>;
    using Buf256k = vban::SpscRingBuffer<float, 262144>; // 2^18 VBAN default

    static_assert(Buf2::capacity() == 2, "Buf2 capacity check");
    static_assert(Buf4::capacity() == 4, "Buf4 capacity check");
    static_assert(Buf16::capacity() == 16, "Buf16 capacity check");
    static_assert(Buf256::capacity() == 256, "Buf256 capacity check");
    static_assert(Buf4k::capacity() == 4096, "Buf4k capacity check");
    static_assert(Buf256k::capacity() == 262144, "Buf256k capacity check");

    ASSERT_EQ(Buf2::capacity(), 2U);
    ASSERT_EQ(Buf2::mask(), 1U);
    ASSERT_EQ(Buf256k::capacity(), 262144U);
    ASSERT_EQ(Buf256k::mask(), 262143U);

    // Verify 64-byte cache line alignment
    ASSERT_GE(alignof(Buf256k), 64U);
    ASSERT_TRUE(std::atomic<uint64_t>::is_always_lock_free);
}

// ==============================================================================
// 2. Initial State & Invariant Checks
// ==============================================================================
TEST_CASE("spsc_buffer_initial_state_and_empty_check") {
    vban::SpscRingBuffer<float, 1024> buf;

    ASSERT_TRUE(buf.empty());
    ASSERT_FALSE(buf.full());
    ASSERT_EQ(buf.availableRead(), 0U);
    ASSERT_EQ(buf.availableWrite(), 1024U);
    ASSERT_NEAR(buf.occupancyPercent(), 0.0f, 1e-6f);
    ASSERT_NEAR(buf.occupancyRatio(), 0.0f, 1e-6f);

    // Reading from empty buffer must return 0
    float dummy[16] = {0};
    size_t read_bytes = buf.read(dummy, 16);
    ASSERT_EQ(read_bytes, 0U);

    // Nullptr or zero count check
    ASSERT_EQ(buf.write(nullptr, 10), 0U);
    ASSERT_EQ(buf.write(dummy, 0), 0U);
    ASSERT_EQ(buf.read(nullptr, 10), 0U);
    ASSERT_EQ(buf.read(dummy, 0), 0U);
}

// ==============================================================================
// 3. Basic Write and Read Operations (FIFO Ordering)
// ==============================================================================
TEST_CASE("spsc_buffer_basic_write_read_fifo") {
    vban::SpscRingBuffer<float, 64> buf;

    std::vector<float> input = {0.1f, -0.2f, 0.35f, -0.7f, 1.0f};
    size_t written = buf.write(input.data(), input.size());
    ASSERT_EQ(written, input.size());
    ASSERT_EQ(buf.availableRead(), input.size());
    ASSERT_EQ(buf.availableWrite(), 64U - input.size());
    ASSERT_FALSE(buf.empty());
    ASSERT_FALSE(buf.full());

    std::vector<float> output(input.size(), 0.0f);
    size_t read_count = buf.read(output.data(), output.size());
    ASSERT_EQ(read_count, input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        ASSERT_NEAR(output[i], input[i], 1e-6f);
    }

    ASSERT_TRUE(buf.empty());
    ASSERT_EQ(buf.availableRead(), 0U);
    ASSERT_EQ(buf.availableWrite(), 64U);
}

// ==============================================================================
// 4. Full Buffer Protection & Boundary Handling
// ==============================================================================
TEST_CASE("spsc_buffer_full_capacity_boundary_and_rejection") {
    constexpr size_t kCap = 16;
    vban::SpscRingBuffer<int, kCap> buf;

    std::vector<int> data(kCap);
    for (size_t i = 0; i < kCap; ++i) {
        data[i] = static_cast<int>(i * 10);
    }

    // Write full capacity
    size_t written = buf.write(data.data(), kCap);
    ASSERT_EQ(written, kCap);
    ASSERT_TRUE(buf.full());
    ASSERT_FALSE(buf.empty());
    ASSERT_EQ(buf.availableWrite(), 0U);
    ASSERT_EQ(buf.availableRead(), kCap);
    ASSERT_NEAR(buf.occupancyPercent(), 100.0f, 1e-6f);

    // Overflow attempt must be rejected non-blockingly (return 0)
    int extra[4] = {991, 992, 993, 994};
    size_t overflow_written = buf.write(extra, 4);
    ASSERT_EQ(overflow_written, 0U);

    // Partial read
    int out[4] = {0};
    size_t read_cnt = buf.read(out, 4);
    ASSERT_EQ(read_cnt, 4U);
    ASSERT_FALSE(buf.full());
    ASSERT_EQ(buf.availableWrite(), 4U);
    ASSERT_EQ(buf.availableRead(), kCap - 4U);

    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(out[i], static_cast<int>(i * 10));
    }

    // Write exactly 4 to fill back up
    size_t refill = buf.write(extra, 4);
    ASSERT_EQ(refill, 4U);
    ASSERT_TRUE(buf.full());
    ASSERT_EQ(buf.availableWrite(), 0U);
}

// ==============================================================================
// 5. Circular Wraparound & Two-Segment Masking
// ==============================================================================
TEST_CASE("spsc_buffer_circular_wrap_across_mask_boundary") {
    constexpr size_t kCap = 8;
    vban::SpscRingBuffer<uint32_t, kCap> buf;

    // Advance write/read indices by 5 items
    uint32_t warm[5] = {1, 2, 3, 4, 5};
    ASSERT_EQ(buf.write(warm, 5), 5U);
    uint32_t warm_out[5] = {0};
    ASSERT_EQ(buf.read(warm_out, 5), 5U);

    // Write index is now 5.
    // Capacity is 8. Writing 6 items will write slots 5, 6, 7 (chunk 1)
    // and wrap across boundary to slots 0, 1, 2 (chunk 2).
    uint32_t wrap_in[6] = {100, 200, 300, 400, 500, 600};
    ASSERT_EQ(buf.write(wrap_in, 6), 6U);
    ASSERT_EQ(buf.availableRead(), 6U);

    uint32_t wrap_out[6] = {0};
    ASSERT_EQ(buf.read(wrap_out, 6), 6U);

    for (int i = 0; i < 6; ++i) {
        ASSERT_EQ(wrap_out[i], wrap_in[i]);
    }

    // Run 100 consecutive wrapping revolutions to guarantee no index drift
    for (uint32_t rev = 0; rev < 100; ++rev) {
        uint32_t burst[5];
        for (uint32_t j = 0; j < 5; ++j) {
            burst[j] = rev * 1000 + j;
        }
        ASSERT_EQ(buf.write(burst, 5), 5U);
        uint32_t burst_out[5] = {0};
        ASSERT_EQ(buf.read(burst_out, 5), 5U);
        for (uint32_t j = 0; j < 5; ++j) {
            ASSERT_EQ(burst_out[j], burst[j]);
        }
    }
}

// ==============================================================================
// 6. Modern C++20 std::span Overloads
// ==============================================================================
TEST_CASE("spsc_buffer_std_span_overloads") {
    vban::SpscRingBuffer<float, 32> buf;

    std::vector<float> input = {1.1f, 2.2f, 3.3f, 4.4f};
    std::span<const float> in_span(input);
    size_t w = buf.write(in_span);
    ASSERT_EQ(w, 4U);

    std::vector<float> output(4, 0.0f);
    std::span<float> out_span(output);
    size_t r = buf.read(out_span);
    ASSERT_EQ(r, 4U);

    for (size_t i = 0; i < 4; ++i) {
        ASSERT_NEAR(output[i], input[i], 1e-6f);
    }
}

// ==============================================================================
// 7. Reset Functionality
// ==============================================================================
TEST_CASE("spsc_buffer_reset_restores_quiescent_state") {
    vban::SpscRingBuffer<float, 64> buf;

    float data[20] = {1.0f};
    buf.write(data, 20);
    ASSERT_EQ(buf.availableRead(), 20U);
    ASSERT_FALSE(buf.empty());

    buf.reset();

    ASSERT_TRUE(buf.empty());
    ASSERT_FALSE(buf.full());
    ASSERT_EQ(buf.availableRead(), 0U);
    ASSERT_EQ(buf.availableWrite(), 64U);

    // Verify normal write/read after reset
    float fresh[5] = {9.0f, 8.0f, 7.0f, 6.0f, 5.0f};
    ASSERT_EQ(buf.write(fresh, 5), 5U);
    ASSERT_EQ(buf.availableRead(), 5U);

    float read_back[5] = {0};
    ASSERT_EQ(buf.read(read_back, 5), 5U);
    for (int i = 0; i < 5; ++i) {
        ASSERT_NEAR(read_back[i], fresh[i], 1e-6f);
    }
}

// ==============================================================================
// 8. Heavy Multi-Threaded Concurrency Stress Test
// ==============================================================================
TEST_CASE("spsc_buffer_multithreaded_stress_5m_samples") {
    constexpr size_t kTotalSamples = 5000000; // 5 Million samples
    vban::SpscRingBuffer<uint32_t, 4096> buffer;

    auto start_time = std::chrono::high_resolution_clock::now();

    // Producer Thread
    std::thread producer([&]() {
        uint32_t current = 0;
        uint32_t chunk[256];
        while (current < kTotalSamples) {
            size_t n = std::min<size_t>(256, kTotalSamples - current);
            for (size_t i = 0; i < n; ++i) {
                chunk[i] = current + static_cast<uint32_t>(i);
            }
            size_t written = buffer.write(chunk, n);
            current += static_cast<uint32_t>(written);
            if (written == 0) {
                std::this_thread::yield();
            }
        }
    });

    // Consumer Thread
    std::vector<uint32_t> received;
    received.reserve(kTotalSamples);

    std::thread consumer([&]() {
        uint32_t chunk[256];
        while (received.size() < kTotalSamples) {
            size_t count = buffer.read(chunk, 256);
            for (size_t i = 0; i < count; ++i) {
                received.push_back(chunk[i]);
            }
            if (count == 0) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
    double throughput_m_samples_per_sec = (static_cast<double>(kTotalSamples) / elapsed_sec) / 1e6;

    // Verify 100% integrity: zero loss, zero reordering, zero corruption
    ASSERT_EQ(received.size(), kTotalSamples);
    for (size_t i = 0; i < kTotalSamples; ++i) {
        if (received[i] != static_cast<uint32_t>(i)) {
            ASSERT_EQ(received[i], static_cast<uint32_t>(i));
        }
    }

    // High throughput requirement: lock-free SPSC buffer must exceed 50M samples/sec
    ASSERT_GT(throughput_m_samples_per_sec, 50.0);
}

// ==============================================================================
// 9. Audiophile Sizing Latency Benchmark (262,144 Frames Capacity)
// ==============================================================================
TEST_CASE("spsc_buffer_audiophile_vban_latency_benchmark") {
    // Exact sizing from PROJECT.md: 262,144 frames (341.3 ms at 768 kHz)
    constexpr size_t kCapacity = 262144;
    vban::SpscRingBuffer<float, kCapacity> buffer;

    // Simulate 10,000 VBAN packets of 256 frames each
    constexpr size_t kPackets = 10000;
    constexpr size_t kPacketSize = 256;
    std::vector<float> packet(kPacketSize, 0.707f);
    std::vector<float> drain(kPacketSize, 0.0f);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t p = 0; p < kPackets; ++p) {
        size_t w = buffer.write(packet.data(), kPacketSize);
        ASSERT_EQ(w, kPacketSize);
        size_t r = buffer.read(drain.data(), kPacketSize);
        ASSERT_EQ(r, kPacketSize);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    double us_per_packet = total_us / (kPackets * 2); // 2 operations per iteration (write + read)

    // A real-time audio packet transfer must take < 5 microseconds (target < 1 us)
    ASSERT_LT(us_per_packet, 5.0);
}
