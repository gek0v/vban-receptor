"""Tier 1: Audio Dequantizers & Format Conversion Tests.

Covers Features:
- Feature 14: PCM 16-bit Dequantization (1.0 / 32768.0)
- Feature 15: PCM 24-bit Packed Dequantization (Sign extension & 1.0 / 8388608.0)
- Feature 16: PCM 32-bit Dequantization (1.0 / 2147483648.0)
- Feature 17: Float32 IEEE 754 Normalization & Sanitization (Clamp [-1.0, 1.0], NaN/Inf -> 0.0)
- Feature 18: Channel Deinterleaving / Interleaving
"""

import math
import struct
import unittest
from e2e_tests.audio_dsp_oracle import AudioDspOracle
from e2e_tests.vban_packet_factory import (
    VbanPacketFactory,
    VBAN_DATATYPE_INT16,
    VBAN_DATATYPE_INT24,
    VBAN_DATATYPE_INT32,
    VBAN_DATATYPE_FLOAT32,
    VBAN_DATATYPE_BYTE8,
)


class TestDequantizers(unittest.TestCase):
    """Tier 1 tests for audio format conversions and channel interleaving."""

    def test_pcm16_normalization_scale(self):
        """Feature 14: Verify 16-bit signed LE conversion to Float32 [-1.0f, +1.0f]."""
        # Exact mathematical scale: 1.0 / 32768.0 = 3.0517578125e-5
        self.assertAlmostEqual(AudioDspOracle.SCALE_INT16, 1.0 / 32768.0, places=9)

        # Test zero
        raw_zero = struct.pack("<h", 0)
        self.assertEqual(AudioDspOracle.dequantize_int16(raw_zero), [0.0])

        # Test positive half-scale (16384 -> +0.5f)
        raw_half = struct.pack("<h", 16384)
        self.assertEqual(AudioDspOracle.dequantize_int16(raw_half), [0.5])

        # Test negative half-scale (-16384 -> -0.5f)
        raw_neg_half = struct.pack("<h", -16384)
        self.assertEqual(AudioDspOracle.dequantize_int16(raw_neg_half), [-0.5])

        # Test full scale min (-32768 -> -1.0f)
        raw_min = struct.pack("<h", -32768)
        self.assertEqual(AudioDspOracle.dequantize_int16(raw_min), [-1.0])

        # Test max positive (+32767 -> +32767.0 / 32768.0 = +0.999969482421875f)
        raw_max = struct.pack("<h", 32767)
        expected_max = 32767.0 / 32768.0
        self.assertAlmostEqual(AudioDspOracle.dequantize_int16(raw_max)[0], expected_max, places=7)

    def test_pcm24_packed_sign_extension(self):
        """Feature 15: Verify 24-bit 3-byte packed signed LE unpacking with proper sign extension."""
        self.assertAlmostEqual(AudioDspOracle.SCALE_INT24, 1.0 / 8388608.0, places=12)

        # Zero (0x000000 -> 0.0f)
        raw_zero = bytes([0x00, 0x00, 0x00])
        self.assertEqual(AudioDspOracle.dequantize_int24_packed(raw_zero), [0.0])

        # Positive half-scale (+4194304 = 0x400000 -> +0.5f)
        # In LE: b0=0x00, b1=0x00, b2=0x40
        raw_half = bytes([0x00, 0x00, 0x40])
        self.assertEqual(AudioDspOracle.dequantize_int24_packed(raw_half), [0.5])

        # Negative half-scale (-4194304 = 0xC00000 -> -0.5f)
        # In LE: b0=0x00, b1=0x00, b2=0xC0
        raw_neg_half = bytes([0x00, 0x00, 0xC0])
        self.assertEqual(AudioDspOracle.dequantize_int24_packed(raw_neg_half), [-0.5])

        # Max negative (-8388608 = 0x800000 -> -1.0f)
        # In LE: b0=0x00, b1=0x00, b2=0x80
        raw_min = bytes([0x00, 0x00, 0x80])
        self.assertEqual(AudioDspOracle.dequantize_int24_packed(raw_min), [-1.0])

        # Max positive (+8388607 = 0x7FFFFF -> +8388607 / 8388608 approx 0.99999988f)
        # In LE: b0=0xFF, b1=0xFF, b2=0x7F
        raw_max = bytes([0xFF, 0xFF, 0x7F])
        expected_max = 8388607.0 / 8388608.0
        self.assertAlmostEqual(AudioDspOracle.dequantize_int24_packed(raw_max)[0], expected_max, places=8)

    def test_pcm32_normalization_scale(self):
        """Feature 16: Verify 32-bit signed LE conversion to Float32 [-1.0f, +1.0f]."""
        self.assertAlmostEqual(AudioDspOracle.SCALE_INT32, 1.0 / 2147483648.0, places=15)

        # Zero
        raw_zero = struct.pack("<i", 0)
        self.assertEqual(AudioDspOracle.dequantize_int32(raw_zero), [0.0])

        # Half scale (+1073741824 -> +0.5f)
        raw_half = struct.pack("<i", 1073741824)
        self.assertEqual(AudioDspOracle.dequantize_int32(raw_half), [0.5])

        # Negative full scale (-2147483648 -> -1.0f)
        raw_min = struct.pack("<i", -2147483648)
        self.assertEqual(AudioDspOracle.dequantize_int32(raw_min), [-1.0])

        # Positive max (+2147483647 -> ~0.9999999995f)
        raw_max = struct.pack("<i", 2147483647)
        self.assertAlmostEqual(AudioDspOracle.dequantize_int32(raw_max)[0], 2147483647.0 / 2147483648.0, places=7)

    def test_float32_passthrough_clamping(self):
        """Feature 17: Verify Float32 IEEE-754 passthrough, sanitization, and clamping."""
        # Valid float samples
        samples = [0.0, 0.25, -0.75, 1.0, -1.0]
        raw_valid = struct.pack(f"<{len(samples)}f", *samples)
        dequantized = AudioDspOracle.dequantize_float32(raw_valid)
        self.assertEqual(dequantized, samples)

        # Out-of-bounds samples should be clamped to [-1.0f, +1.0f]
        raw_outliers = struct.pack("<2f", 2.5, -3.14)
        clamped = AudioDspOracle.dequantize_float32(raw_outliers)
        self.assertEqual(clamped, [1.0, -1.0])

        # NaN and Infinity should be sanitized to 0.0f
        raw_corrupt = struct.pack("<4f", float("nan"), float("inf"), float("-inf"), 0.5)
        sanitized = AudioDspOracle.dequantize_float32(raw_corrupt)
        self.assertEqual(sanitized, [0.0, 0.0, 0.0, 0.5])

    def test_channel_interleave_layout(self):
        """Feature 18: Verify multi-channel deinterleaving and interleaving consistency."""
        # Stereo layout: [L0, R0, L1, R1, L2, R2]
        interleaved_stereo = [0.1, -0.1, 0.2, -0.2, 0.3, -0.3]
        channels_split = AudioDspOracle.deinterleave_channels(interleaved_stereo, 2)
        self.assertEqual(len(channels_split), 2)
        self.assertEqual(channels_split[0], [0.1, 0.2, 0.3])   # Left channel
        self.assertEqual(channels_split[1], [-0.1, -0.2, -0.3]) # Right channel

        # Round-trip reconstruction
        recombined = AudioDspOracle.interleave_channels(channels_split)
        self.assertEqual(recombined, interleaved_stereo)

        # 6-Channel (5.1 Surround) layout: [FL, FR, C, LFE, SL, SR]
        ch6_data = [
            [1.0, 1.1],  # FL
            [2.0, 2.1],  # FR
            [3.0, 3.1],  # C
            [4.0, 4.1],  # LFE
            [5.0, 5.1],  # SL
            [6.0, 6.1],  # SR
        ]
        interleaved_51 = AudioDspOracle.interleave_channels(ch6_data)
        self.assertEqual(interleaved_51, [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 1.1, 2.1, 3.1, 4.1, 5.1, 6.1])
        deinterleaved_51 = AudioDspOracle.deinterleave_channels(interleaved_51, 6)
        self.assertEqual(deinterleaved_51, ch6_data)

    def test_byte8_unsigned_conversion(self):
        """Verify 8-bit unsigned integer (bias 128) conversion."""
        raw_byte8 = bytes([0, 64, 128, 192, 255])
        converted = AudioDspOracle.dequantize_payload(raw_byte8, VBAN_DATATYPE_BYTE8)
        self.assertEqual(converted[0], -1.0)       # (0 - 128)/128 = -1.0
        self.assertEqual(converted[1], -0.5)       # (64 - 128)/128 = -0.5
        self.assertEqual(converted[2], 0.0)        # (128 - 128)/128 = 0.0
        self.assertEqual(converted[3], 0.5)        # (192 - 128)/128 = 0.5
        self.assertAlmostEqual(converted[4], 127.0 / 128.0, places=6)


if __name__ == "__main__":
    unittest.main()
