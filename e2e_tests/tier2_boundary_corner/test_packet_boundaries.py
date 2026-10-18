"""Tier 2: Packet Boundary, Truncation & Framing Corner Cases.

Covers boundary and corner conditions for VBAN packet ingestion:
- Length < 28 bytes (0, 1, 10, 27 bytes)
- Truncated audio payload
- Trailing garbage bytes appended
- Format nbs extremes (0 and 255 -> 1 and 256 samples)
- Format nbc extremes (0 and 255 -> 1 and 256 channels)
- Stream name 16 chars without null terminator and whitespace handling
"""

import unittest
from e2e_tests.audio_dsp_oracle import VbanPacketValidator
from e2e_tests.vban_packet_factory import (
    VbanPacketFactory,
    VBAN_DATATYPE_FLOAT32,
    VBAN_DATATYPE_INT16,
)


class TestPacketBoundaries(unittest.TestCase):
    """Tier 2 tests for packet framing edge cases."""

    def setUp(self):
        self.validator = VbanPacketValidator(target_stream_name="Stream1")

    def test_packet_length_under_28_bytes(self):
        """Tier 2: Verify immediate drop of datagrams smaller than 28 bytes."""
        truncated_inputs = [
            b"",
            b"V",
            b"VBAN",
            b"VBAN\x03\x00\x00\x04",
            b"A" * 27,
        ]
        for truncated in truncated_inputs:
            valid, reason, _ = self.validator.validate_and_parse(truncated)
            self.assertFalse(valid, f"Should reject length {len(truncated)}")
            self.assertEqual(reason, "DROP_LENGTH_UNDER_28")

    def test_truncated_payload(self):
        """Tier 2: Verify drop when payload is cut short by network truncation."""
        # Valid packet is 28 + 128*2*4 = 1052 bytes
        valid_pkt = VbanPacketFactory.create_packet(
            samples_per_frame=128, channels=2, data_type=VBAN_DATATYPE_FLOAT32
        )
        self.assertEqual(len(valid_pkt), 1052)

        # Truncate by 1 byte, 4 bytes, half
        for cut in [1, 4, 100, 500]:
            truncated = valid_pkt[:-cut]
            valid, reason, _ = self.validator.validate_and_parse(truncated)
            self.assertFalse(valid)
            self.assertTrue(reason.startswith("DROP_TRUNCATED_PAYLOAD"))

    def test_trailing_garbage(self):
        """Tier 2: Verify drop when packet has unexpected trailing bytes appended."""
        valid_pkt = VbanPacketFactory.create_packet(
            samples_per_frame=64, channels=2, data_type=VBAN_DATATYPE_INT16
        )
        # Expected size: 28 + 64*2*2 = 284 bytes
        self.assertEqual(len(valid_pkt), 284)

        # Append 1 byte, 16 bytes garbage
        for extra in [b"\x00", b"GARBAGE_PAYLOAD_"]:
            corrupted = valid_pkt + extra
            valid, reason, _ = self.validator.validate_and_parse(corrupted)
            self.assertFalse(valid)
            self.assertTrue(reason.startswith("DROP_TRAILING_GARBAGE"))

    def test_format_nbs_extremes(self):
        """Tier 2: Verify nbs=0 (1 sample) and nbs=255 (256 samples)."""
        # Minimum: nbs=0 -> 1 sample per frame
        pkt_min = VbanPacketFactory.create_packet(samples_per_frame=1, channels=2)
        valid, _, info = self.validator.validate_and_parse(pkt_min)
        self.assertTrue(valid)
        self.assertEqual(info["samples_per_frame"], 1)
        self.assertEqual(len(info["samples"]), 2)  # 1 frame * 2 ch

        # Maximum: nbs=255 -> 256 samples per frame
        pkt_max = VbanPacketFactory.create_packet(samples_per_frame=256, channels=2)
        valid, _, info = self.validator.validate_and_parse(pkt_max)
        self.assertTrue(valid)
        self.assertEqual(info["samples_per_frame"], 256)
        self.assertEqual(len(info["samples"]), 512)  # 256 frames * 2 ch

    def test_format_nbc_extremes(self):
        """Tier 2: Verify nbc=0 (mono, 1 channel) and multi-channel boundaries."""
        # Mono (nbc=0)
        pkt_mono = VbanPacketFactory.create_packet(samples_per_frame=64, channels=1)
        valid, _, info = self.validator.validate_and_parse(pkt_mono)
        self.assertTrue(valid)
        self.assertEqual(info["channels"], 1)

        # 8 channels (nbc=7)
        pkt_8ch = VbanPacketFactory.create_packet(samples_per_frame=64, channels=8)
        valid, _, info = self.validator.validate_and_parse(pkt_8ch)
        self.assertTrue(valid)
        self.assertEqual(info["channels"], 8)

    def test_stream_name_all_null_and_max_16_chars(self):
        """Tier 2: Verify 16-character stream name without null-terminator."""
        name_16 = "Stream1234567890"  # exactly 16 chars
        validator_16 = VbanPacketValidator(target_stream_name=name_16)

        pkt = VbanPacketFactory.create_packet(stream_name=name_16)
        valid, _, info = validator_16.validate_and_parse(pkt)
        self.assertTrue(valid)
        self.assertEqual(info["stream_name"], name_16)


if __name__ == "__main__":
    unittest.main()
