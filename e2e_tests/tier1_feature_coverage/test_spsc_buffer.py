"""Tier 1: Lock-Free SPSC Circular Buffer Tests.

Covers Feature:
- Feature 19: Lock-Free SPSC Circular Buffer (2^18 frames = 262,144 frames,
  zero allocations, monotonic 64-bit indices, power-of-two mask indexing,
  concurrency safety, underrun silence fill).
"""

import threading
import time
import unittest
from e2e_tests.spsc_ring_buffer_sim import SpscRingBufferSim


class TestSpscBuffer(unittest.TestCase):
    """Tier 1 tests for SPSC ring buffer invariants."""

    def test_spsc_power_of_two_capacity(self):
        """Feature 19: Verify power-of-two capacity (2^18 = 262,144 frames = 341.3 ms at 768 kHz)."""
        buf = SpscRingBufferSim(capacity=262144, channels=2)
        self.assertEqual(buf.capacity, 262144)
        self.assertEqual(buf.capacity, 2**18)
        self.assertEqual(buf.mask, 262143)
        self.assertEqual(buf.available_read(), 0)
        self.assertEqual(buf.available_write(), 262144)
        self.assertEqual(buf.occupancy_percent(), 0.0)

        # Invalid non-power-of-two should assert
        with self.assertRaises(AssertionError):
            SpscRingBufferSim(capacity=100000)

    def test_spsc_non_blocking_write_and_read(self):
        """Feature 19: Verify non-blocking push/pull of interleaved frames."""
        buf = SpscRingBufferSim(capacity=1024, channels=2)

        # Push 128 stereo frames
        test_data = [float(i) * 0.001 for i in range(128 * 2)]
        written = buf.write(test_data, 128)
        self.assertEqual(written, 128)
        self.assertEqual(buf.available_read(), 128)
        self.assertEqual(buf.available_write(), 1024 - 128)

        # Pull 64 stereo frames
        read_frames, actual_pulled = buf.read(64)
        self.assertEqual(actual_pulled, 64)
        self.assertEqual(len(read_frames), 64 * 2)
        self.assertEqual(read_frames, test_data[: 64 * 2])
        self.assertEqual(buf.available_read(), 64)

        # Pull remaining 64 frames
        read_frames2, actual_pulled2 = buf.read(64)
        self.assertEqual(actual_pulled2, 64)
        self.assertEqual(read_frames2, test_data[64 * 2 : 128 * 2])
        self.assertEqual(buf.available_read(), 0)

    def test_spsc_underrun_silence_fill(self):
        """Feature 19: Verify buffer underrun handling fills output with 0.0f silence."""
        buf = SpscRingBufferSim(capacity=512, channels=2)

        # Buffer only has 10 frames
        short_data = [0.5] * (10 * 2)
        buf.write(short_data, 10)

        # Callback requests 32 frames (22 frames underrun)
        out, pulled = buf.read(32)
        self.assertEqual(pulled, 10)
        self.assertEqual(buf.underrun_count, 1)
        self.assertEqual(buf.underrun_silence_frames, 22)

        # First 10 frames are 0.5, remaining 22 frames are digital silence (0.0f)
        self.assertEqual(out[:20], [0.5] * 20)
        self.assertEqual(out[20:], [0.0] * 44)

    def test_spsc_overflow_dropped_frames(self):
        """Feature 19: Verify buffer full condition drops excess frames without blocking or memory corruption."""
        buf = SpscRingBufferSim(capacity=256, channels=2)

        # Fill entire buffer
        data = [0.1] * (256 * 2)
        written = buf.write(data, 256)
        self.assertEqual(written, 256)
        self.assertEqual(buf.available_write(), 0)

        # Attempt to write 50 more frames to full buffer
        extra = [0.2] * (50 * 2)
        written_extra = buf.write(extra, 50)
        self.assertEqual(written_extra, 0)
        self.assertEqual(buf.overflow_dropped_frames, 50)

    def test_spsc_watermark_and_occupancy(self):
        """Feature 19: Verify occupancy percentage computation."""
        with self.assertRaises(AssertionError):
            SpscRingBufferSim(capacity=1000, channels=2)

        buf2 = SpscRingBufferSim(capacity=1024, channels=2)
        buf2.write([0.0] * (512 * 2), 512)
        self.assertEqual(buf2.occupancy_percent(), 50.0)

        buf2.read(256)
        self.assertEqual(buf2.occupancy_percent(), 25.0)

    def test_spsc_concurrent_producer_consumer(self):
        """Feature 19: Multi-threaded producer and consumer concurrency verification."""
        capacity = 8192
        buf = SpscRingBufferSim(capacity=capacity, channels=2)
        total_test_frames = 20000
        burst_size = 128

        producer_finished = threading.Event()
        received_frames_count = [0]

        def producer_worker():
            frames_sent = 0
            while frames_sent < total_test_frames:
                free = buf.available_write()
                if free > 0:
                    to_send = min(burst_size, min(free, total_test_frames - frames_sent))
                    chunk = [float((frames_sent + i // 2) % 1000) * 0.001 for i in range(to_send * 2)]
                    written = buf.write(chunk, to_send)
                    frames_sent += written
                else:
                    time.sleep(0.0001)  # Yield if buffer full
            producer_finished.set()

        def consumer_worker():
            while not producer_finished.is_set() or buf.available_read() > 0:
                avail = buf.available_read()
                if avail > 0:
                    read_count = min(burst_size, avail)
                    frames, pulled = buf.read(read_count)
                    received_frames_count[0] += pulled
                else:
                    time.sleep(0.0001)

        t_prod = threading.Thread(target=producer_worker)
        t_cons = threading.Thread(target=consumer_worker)

        t_prod.start()
        t_cons.start()
        t_prod.join(timeout=5.0)
        t_cons.join(timeout=5.0)

        self.assertFalse(t_prod.is_alive())
        self.assertFalse(t_cons.is_alive())
        self.assertEqual(received_frames_count[0], total_test_frames)
        self.assertEqual(buf.overflow_dropped_frames, 0)


if __name__ == "__main__":
    unittest.main()
