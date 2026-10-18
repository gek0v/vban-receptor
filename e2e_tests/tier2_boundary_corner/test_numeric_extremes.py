"""Tier 2: Numeric Extremes, IEEE-754 Anomalies & Clamping Corner Cases.

Covers:
- 24-bit packed signed boundaries (0x800000 = -8,388,608, 0x7FFFFF = +8,388,607)
- 16-bit signed boundaries (-32768, +32767)
- 32-bit signed boundaries (-2147483648, +2147483647)
- IEEE-754 NaN and Infinity sanitization (replaced with 0.0f)
- Float32 outlier clamping (values outside [-1.0f, +1.0f])
"""

import struct
import unittest
from e2e_tests.audio_dsp_oracle import AudioDspOracle


class TestNumericExtremes(unittest.TestCase):
    """Tier 2 tests for numeric edge conditions and sanitization."""

    def test_24bit_extreme_values(self):
        """Tier 2: Verify exact mathematical boundaries of 24-bit packed integers."""
        # 1. Negative full scale: 0x800000 (-8,388,608) -> -1.0f
        b_min = bytes([0x00, 0x00, 0x80])
        val_min = AudioDspOracle.dequantize_int24_packed(b_min)[0]
        self.assertEqual(val_min, -1.0)

        # 2. Positive full scale: 0x7FFFFF (+8,388,607) -> +8388607 / 8388608
        b_max = bytes([0xFF, 0xFF, 0x7F])
        val_max = AudioDspOracle.dequantize_int24_packed(b_max)[0]
        self.assertAlmostEqual(val_max, 8388607.0 / 8388608.0, places=8)
        self.assertLess(val_max, 1.0)
        self.assertGreater(val_max, 0.999999)

        # 3. Minus one: 0xFFFFFF (-1) -> -1.0 / 8388608.0
        b_minus_one = bytes([0xFF, 0xFF, 0xFF])
        val_m1 = AudioDspOracle.dequantize_int24_packed(b_minus_one)[0]
        self.assertAlmostEqual(val_m1, -1.0 / 8388608.0, places=9)

        # 4. Plus one: 0x000001 (+1) -> +1.0 / 8388608.0
        b_plus_one = bytes([0x01, 0x00, 0x00])
        val_p1 = AudioDspOracle.dequantize_int24_packed(b_plus_one)[0]
        self.assertAlmostEqual(val_p1, 1.0 / 8388608.0, places=9)

    def test_16bit_extreme_values(self):
        """Tier 2: Verify exact mathematical boundaries of 16-bit integers."""
        # Negative full scale: -32768 -> -1.0f
        val_min = AudioDspOracle.dequantize_int16(struct.pack("<h", -32768))[0]
        self.assertEqual(val_min, -1.0)

        # Positive full scale: +32767 -> +32767 / 32768
        val_max = AudioDspOracle.dequantize_int16(struct.pack("<h", 32767))[0]
        self.assertAlmostEqual(val_max, 32767.0 / 32768.0, places=7)

        # -1 and +1
        val_m1 = AudioDspOracle.dequantize_int16(struct.pack("<h", -1))[0]
        self.assertAlmostEqual(val_m1, -1.0 / 32768.0, places=8)
        val_p1 = AudioDspOracle.dequantize_int16(struct.pack("<h", 1))[0]
        self.assertAlmostEqual(val_p1, 1.0 / 32768.0, places=8)

    def test_32bit_extreme_values(self):
        """Tier 2: Verify exact mathematical boundaries of 32-bit integers."""
        # Negative full scale: -2147483648 -> -1.0f
        val_min = AudioDspOracle.dequantize_int32(struct.pack("<i", -2147483648))[0]
        self.assertEqual(val_min, -1.0)

        # Positive full scale: +2147483647 -> +2147483647 / 2147483648
        val_max = AudioDspOracle.dequantize_int32(struct.pack("<i", 2147483647))[0]
        self.assertAlmostEqual(val_max, 2147483647.0 / 2147483648.0, places=7)

    def test_ieee754_special_floats(self):
        """Tier 2: Verify that positive/negative NaN and +/-Inf are replaced with 0.0f."""
        # IEEE 754 bit representations:
        # +NaN: 0x7FC00000, -NaN: 0xFFC00000
        # +Inf: 0x7F800000, -Inf: 0xFF800000
        raw_special = struct.pack("<4I", 0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000)
        sanitized = AudioDspOracle.dequantize_float32(raw_special)
        self.assertEqual(sanitized, [0.0, 0.0, 0.0, 0.0])

    def test_clipping_and_outliers(self):
        """Tier 2: Verify that audio signals beyond [-1.0f, +1.0f] are safely clamped."""
        outlier_floats = [1.0001, 10.0, 999.0, -1.0001, -50.0]
        raw_floats = struct.pack(f"<{len(outlier_floats)}f", *outlier_floats)
        clamped = AudioDspOracle.dequantize_float32(raw_floats)
        self.assertEqual(clamped, [1.0, 1.0, 1.0, -1.0, -1.0])


if __name__ == "__main__":
    unittest.main()
