"""Tier 2: SPSC Circular Buffer Extremes & Overflow/Underrun Corner Cases.

Covers:
- Consumer read on completely empty buffer (100% underrun, silence fill)
- Producer write on completely full buffer (100% overflow drop, zero corruption)
- Single frame push/pull (minimal quantum)
- Exact capacity write (262,144 frames, 100% fill level)
- Continuous wrap-around cycle across 10,000 frames
"""

import unittest
from e2e_tests.spsc_ring_buffer_sim import SpscRingBufferSim


class TestBufferExtremes(unittest.TestCase):
    """Tier 2 tests for ring buffer boundary states."""

    def test_read_on_completely_empty_buffer(self):
        """Tier 2: Verify read on completely empty buffer generates 100% silence."""
        buf = SpscRingBufferSim(capacity=1024, channels=2)
        self.assertEqual(buf.available_read(), 0)

        out, pulled = buf.read(128)
        self.assertEqual(pulled, 0)
        self.assertEqual(buf.underrun_count, 1)
        self.assertEqual(buf.underrun_silence_frames, 128)
        self.assertEqual(len(out), 128 * 2)
        # Verify completely silent
        self.assertTrue(all(s == 0.0 for s in out))

    def test_write_on_completely_full_buffer(self):
        """Tier 2: Verify write on completely full buffer drops data safely without memory corruption."""
        buf = SpscRingBufferSim(capacity=256, channels=2)
        data = [0.42] * (256 * 2)
        written = buf.write(data, 256)
        self.assertEqual(written, 256)
        self.assertEqual(buf.available_write(), 0)

        # Attempt to write 64 more frames
        extra = [0.99] * (64 * 2)
        written_extra = buf.write(extra, 64)
        self.assertEqual(written_extra, 0)
        self.assertEqual(buf.overflow_dropped_frames, 64)
        self.assertEqual(buf.available_read(), 256)

        # Existing buffer data is unmodified
        out, pulled = buf.read(256)
        self.assertEqual(pulled, 256)
        self.assertTrue(all(s == 0.42 for s in out))

    def test_single_frame_read_write(self):
        """Tier 2: Verify minimal single-frame push and pull."""
        buf = SpscRingBufferSim(capacity=1024, channels=2)
        written = buf.write([0.123, -0.456], 1)
        self.assertEqual(written, 1)
        self.assertEqual(buf.available_read(), 1)

        out, pulled = buf.read(1)
        self.assertEqual(pulled, 1)
        self.assertEqual(out, [0.123, -0.456])
        self.assertEqual(buf.available_read(), 0)

    def test_exact_capacity_write(self):
        """Tier 2: Verify filling buffer to exact 262,144 frames capacity (100.0% occupancy)."""
        buf = SpscRingBufferSim(capacity=262144, channels=2)
        chunk_size = 32768  # 8 chunks of 32768 = 262144
        dummy_chunk = [0.01] * (chunk_size * 2)

        for _ in range(8):
            buf.write(dummy_chunk, chunk_size)

        self.assertEqual(buf.available_read(), 262144)
        self.assertEqual(buf.available_write(), 0)
        self.assertEqual(buf.occupancy_percent(), 100.0)

    def test_continuous_wrap_around_cycle(self):
        """Tier 2: Verify circular mask indexing through 10,000 single-frame wrap cycles."""
        buf = SpscRingBufferSim(capacity=256, channels=2)

        for i in range(10000):
            val = float(i) * 0.001
            written = buf.write([val, -val], 1)
            self.assertEqual(written, 1)
            out, pulled = buf.read(1)
            self.assertEqual(pulled, 1)
            self.assertEqual(out, [val, -val])

        self.assertEqual(buf.available_read(), 0)
        self.assertEqual(buf.total_frames_written, 10000)
        self.assertEqual(buf.total_frames_read, 10000)
        self.assertEqual(buf.overflow_dropped_frames, 0)
        self.assertEqual(buf.underrun_count, 0)


if __name__ == "__main__":
    unittest.main()
