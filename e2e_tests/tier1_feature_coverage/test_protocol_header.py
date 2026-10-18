"""Tier 1: Protocol Header & Parser Validation Tests.

Covers Features:
- Feature 4: Fixed 28-Byte Header Structure
- Feature 5: FourCC Magic Validation
- Feature 6: Subprotocol Verification
- Feature 7: Official 21 Sample Rates Table
- Feature 8: 768 kHz Sample Rate Extension
- Feature 9: Frame Size (format_nbs) Decoding
- Feature 10: Channel Count (format_nbc) Decoding
- Feature 11: Codec & Reserved Bit 3 Check
- Feature 12: Stream Name Matching
- Feature 13: Exact Payload Size Calculation
"""

import struct
import unittest
from e2e_tests.vban_packet_factory import (
    VbanPacketFactory,
    VBAN_SR_TABLE,
    VBAN_MAGIC_BYTES,
    VBAN_PROTOCOL_AUDIO,
    VBAN_PROTOCOL_SERIAL,
    VBAN_PROTOCOL_TXT,
    VBAN_PROTOCOL_SERVICE,
    VBAN_CODEC_PCM,
    VBAN_CODEC_VBCA,
    VBAN_DATATYPE_INT16,
    VBAN_DATATYPE_INT24,
    VBAN_DATATYPE_FLOAT32,
)
from e2e_tests.audio_dsp_oracle import VbanPacketValidator


class TestProtocolHeader(unittest.TestCase):
    """Tier 1 tests for VBAN protocol header and packet parsing."""

    def setUp(self):
        self.validator = VbanPacketValidator(target_stream_name="Stream1")

    def test_header_size_and_packing(self):
        """Feature 4: Verify header is exactly 28 bytes packed without padding."""
        header = VbanPacketFactory.pack_header(
            magic=VBAN_MAGIC_BYTES,
            subprotocol=VBAN_PROTOCOL_AUDIO,
            sr_index=3,
            nbs=127,
            nbc=1,
            codec=VBAN_CODEC_PCM,
            bit3_reserved=0,
            data_type=VBAN_DATATYPE_FLOAT32,
            stream_name="Stream1",
            nu_frame=42,
        )
        self.assertEqual(len(header), 28)
        # Validate exact field offsets
        magic, format_sr, format_nbs, format_nbc, format_bit, stream_name, nu_frame = struct.unpack(
            "<4sBBBB16sI", header
        )
        self.assertEqual(magic, b"VBAN")
        self.assertEqual(format_sr, 0x03)
        self.assertEqual(format_nbs, 127)
        self.assertEqual(format_nbc, 1)
        self.assertEqual(format_bit, 0x04)
        self.assertEqual(stream_name.rstrip(b"\x00"), b"Stream1")
        self.assertEqual(nu_frame, 42)

    def test_fourcc_magic_vban_valid(self):
        """Feature 5: Verify FourCC magic 'VBAN' acceptance and corrupted magic rejection."""
        # Valid 'VBAN'
        valid_pkt = VbanPacketFactory.create_packet(magic=b"VBAN")
        is_valid, reason, info = self.validator.validate_and_parse(valid_pkt)
        self.assertTrue(is_valid)
        self.assertIsNone(reason)

        # Invalid magic 'RTP\0'
        bad_pkt = VbanPacketFactory.create_packet(magic=b"RTP\x00")
        is_valid, reason, info = self.validator.validate_and_parse(bad_pkt)
        self.assertFalse(is_valid)
        self.assertEqual(reason, "DROP_INVALID_MAGIC")

        # Invalid magic 'NABV' (endian mismatch)
        bad_pkt2 = VbanPacketFactory.create_packet(magic=b"NABV")
        is_valid, reason, info = self.validator.validate_and_parse(bad_pkt2)
        self.assertFalse(is_valid)
        self.assertEqual(reason, "DROP_INVALID_MAGIC")

    def test_subprotocol_audio_enforced(self):
        """Feature 6: Verify only subprotocol 0x00 (Audio) is accepted."""
        # Audio subprotocol (0x00) -> Accepted
        pkt_audio = VbanPacketFactory.create_packet(subprotocol=VBAN_PROTOCOL_AUDIO)
        valid, reason, _ = self.validator.validate_and_parse(pkt_audio)
        self.assertTrue(valid)

        # Serial subprotocol (0x20) -> Rejected
        pkt_serial = VbanPacketFactory.create_packet(subprotocol=VBAN_PROTOCOL_SERIAL)
        valid, reason, _ = self.validator.validate_and_parse(pkt_serial)
        self.assertFalse(valid)
        self.assertEqual(reason, f"DROP_NON_AUDIO_SUBPROTOCOL_{hex(VBAN_PROTOCOL_SERIAL)}")

        # Text subprotocol (0x40) -> Rejected
        pkt_txt = VbanPacketFactory.create_packet(subprotocol=VBAN_PROTOCOL_TXT)
        valid, reason, _ = self.validator.validate_and_parse(pkt_txt)
        self.assertFalse(valid)
        self.assertEqual(reason, f"DROP_NON_AUDIO_SUBPROTOCOL_{hex(VBAN_PROTOCOL_TXT)}")

        # Service subprotocol (0x60) -> Rejected
        pkt_srv = VbanPacketFactory.create_packet(subprotocol=VBAN_PROTOCOL_SERVICE)
        valid, reason, _ = self.validator.validate_and_parse(pkt_srv)
        self.assertFalse(valid)
        self.assertEqual(reason, f"DROP_NON_AUDIO_SUBPROTOCOL_{hex(VBAN_PROTOCOL_SERVICE)}")

    def test_official_21_sample_rates(self):
        """Feature 7: Verify all 21 official sample rates (6 kHz to 705.6 kHz) correctly parse."""
        expected_official_rates = [
            6000, 12000, 24000, 48000, 96000, 192000, 384000,
            8000, 16000, 32000, 64000, 128000, 256000, 512000,
            11025, 22050, 44100, 88200, 176400, 352800, 705600,
        ]
        self.assertEqual(len(expected_official_rates), 21)

        for sr_index, expected_rate in enumerate(expected_official_rates):
            pkt = VbanPacketFactory.create_packet(sample_rate=expected_rate)
            valid, reason, info = self.validator.validate_and_parse(pkt)
            self.assertTrue(valid, f"Failed parsing official rate {expected_rate} Hz (index {sr_index})")
            self.assertEqual(info["sample_rate"], expected_rate)

    def test_768k_sample_rate_extension(self):
        """Feature 8: Verify index 21 extension for audiophile 768 kHz DAC rendering."""
        self.assertEqual(VBAN_SR_TABLE[21], 768000)
        pkt_768k = VbanPacketFactory.create_packet(sample_rate=768000)
        valid, reason, info = self.validator.validate_and_parse(pkt_768k)
        self.assertTrue(valid)
        self.assertEqual(info["sample_rate"], 768000)

    def test_format_nbs_decoding(self):
        """Feature 9: Verify format_nbs decoding (sample count = format_nbs + 1, range 1..256)."""
        test_samples = [1, 64, 128, 256]
        for s in test_samples:
            pkt = VbanPacketFactory.create_packet(samples_per_frame=s)
            valid, reason, info = self.validator.validate_and_parse(pkt)
            self.assertTrue(valid)
            self.assertEqual(info["samples_per_frame"], s)

    def test_format_nbc_decoding(self):
        """Feature 10: Verify format_nbc decoding (channels = format_nbc + 1, mono to 8-channel)."""
        test_channels = [1, 2, 6, 8]  # Mono, Stereo, 5.1 Surround, 7.1 Surround
        for ch in test_channels:
            pkt = VbanPacketFactory.create_packet(channels=ch)
            valid, reason, info = self.validator.validate_and_parse(pkt)
            self.assertTrue(valid)
            self.assertEqual(info["channels"], ch)

    def test_codec_pcm_and_bit3_zero(self):
        """Feature 11: Verify PCM codec 0x00 enforced and bit 3 reserved check enforced."""
        # Standard PCM with bit 3 = 0 -> Valid
        pkt_pcm = VbanPacketFactory.create_packet(codec=VBAN_CODEC_PCM, bit3_reserved=0)
        valid, reason, _ = self.validator.validate_and_parse(pkt_pcm)
        self.assertTrue(valid)

        # Bit 3 set to 1 -> Protocol violation, must be dropped
        pkt_bit3 = VbanPacketFactory.create_packet(codec=VBAN_CODEC_PCM, bit3_reserved=1)
        valid, reason, _ = self.validator.validate_and_parse(pkt_bit3)
        self.assertFalse(valid)
        self.assertEqual(reason, "DROP_RESERVED_BIT3_SET")

        # Compressed codec VBCA (0x10) -> Dropped
        pkt_vbca = VbanPacketFactory.create_packet(codec=VBAN_CODEC_VBCA, bit3_reserved=0)
        valid, reason, _ = self.validator.validate_and_parse(pkt_vbca)
        self.assertFalse(valid)
        self.assertEqual(reason, f"DROP_UNSUPPORTED_CODEC_{hex(VBAN_CODEC_VBCA)}")

    def test_stream_name_matching_ascii(self):
        """Feature 12: Verify 16-byte null-padded stream name extraction and filtering."""
        # Exact match
        pkt_match = VbanPacketFactory.create_packet(stream_name="Stream1")
        valid, reason, info = self.validator.validate_and_parse(pkt_match)
        self.assertTrue(valid)
        self.assertEqual(info["stream_name"], "Stream1")

        # Case-insensitive match
        pkt_case = VbanPacketFactory.create_packet(stream_name="stream1")
        valid, reason, info = self.validator.validate_and_parse(pkt_case)
        self.assertTrue(valid)

        # Unsubscribed stream name -> Dropped
        pkt_other = VbanPacketFactory.create_packet(stream_name="VoicemeeterA1")
        valid, reason, info = self.validator.validate_and_parse(pkt_other)
        self.assertFalse(valid)
        self.assertTrue(reason.startswith("DROP_UNSUBSCRIBED_STREAM"))

    def test_exact_payload_size_valid(self):
        """Feature 13: Verify exact payload byte length validation (28 + nbs * nbc * sampleSize)."""
        # Stereo 16-bit PCM (sample size = 2 bytes), 128 samples: 28 + 128 * 2 * 2 = 540 bytes
        pkt16 = VbanPacketFactory.create_packet(
            samples_per_frame=128, channels=2, data_type=VBAN_DATATYPE_INT16
        )
        self.assertEqual(len(pkt16), 28 + 128 * 2 * 2)
        valid, _, _ = self.validator.validate_and_parse(pkt16)
        self.assertTrue(valid)

        # Stereo 24-bit packed (sample size = 3 bytes), 128 samples: 28 + 128 * 2 * 3 = 796 bytes
        pkt24 = VbanPacketFactory.create_packet(
            samples_per_frame=128, channels=2, data_type=VBAN_DATATYPE_INT24
        )
        self.assertEqual(len(pkt24), 28 + 128 * 2 * 3)
        valid, _, _ = self.validator.validate_and_parse(pkt24)
        self.assertTrue(valid)

        # Stereo Float32 (sample size = 4 bytes), 128 samples: 28 + 128 * 2 * 4 = 1052 bytes
        pkt32 = VbanPacketFactory.create_packet(
            samples_per_frame=128, channels=2, data_type=VBAN_DATATYPE_FLOAT32
        )
        self.assertEqual(len(pkt32), 28 + 128 * 2 * 4)
        valid, _, _ = self.validator.validate_and_parse(pkt32)
        self.assertTrue(valid)


if __name__ == "__main__":
    unittest.main()
