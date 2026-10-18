"""Tier 2: Sequence Number Wraparound & Packet Loss Burst Corner Cases.

Covers:
- nuFrame 32-bit unsigned rollover (0xFFFFFFFF -> 0x00000000)
- nuFrame rollover with gap (0xFFFFFFFE -> 0x00000002)
- Massive packet loss burst (10,000 lost packets)
- Duplicate frame reception (delta == 0)
- Late out-of-order packet arrival (delta < 0)
"""

import unittest
from e2e_tests.audio_dsp_oracle import VbanPacketValidator
from e2e_tests.vban_packet_factory import VbanPacketFactory


class TestSequenceWraparound(unittest.TestCase):
    """Tier 2 tests for sequence counter modular arithmetic and packet loss detection."""

    def test_nuframe_32bit_wraparound(self):
        """Tier 2: Verify seamless sequence continuity across 32-bit uint wrap (0xFFFFFFFF -> 0x00000000)."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        # Packet at maximum uint32
        pkt_max = VbanPacketFactory.create_packet(nu_frame=0xFFFFFFFF)
        valid, _, _ = validator.validate_and_parse(pkt_max)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 0)

        # Immediate next packet rolls over to 0
        pkt_zero = VbanPacketFactory.create_packet(nu_frame=0x00000000)
        valid, _, _ = validator.validate_and_parse(pkt_zero)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 0, "Sequential rollover should not report packet loss")

        # Next packet
        pkt_one = VbanPacketFactory.create_packet(nu_frame=0x00000001)
        valid, _, _ = validator.validate_and_parse(pkt_one)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 0)

    def test_nuframe_wrap_with_gap(self):
        """Tier 2: Verify loss counting across 32-bit rollover."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        # Packet at 0xFFFFFFFE
        pkt1 = VbanPacketFactory.create_packet(nu_frame=0xFFFFFFFE)
        validator.validate_and_parse(pkt1)

        # Next arriving packet is 0x00000002
        # Missing packets: 0xFFFFFFFF, 0x00000000, 0x00000001 (total 3 lost packets)
        pkt2 = VbanPacketFactory.create_packet(nu_frame=0x00000002)
        valid, _, _ = validator.validate_and_parse(pkt2)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 3)

    def test_large_burst_loss(self):
        """Tier 2: Verify detection of a large packet loss burst (e.g. 10,000 lost packets)."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        pkt_start = VbanPacketFactory.create_packet(nu_frame=500)
        validator.validate_and_parse(pkt_start)

        pkt_after_burst = VbanPacketFactory.create_packet(nu_frame=10501)
        valid, _, _ = validator.validate_and_parse(pkt_after_burst)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 10000)

    def test_duplicate_packet(self):
        """Tier 2: Verify duplicate packet arrival (delta == 0) increments out-of-order counter."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        pkt = VbanPacketFactory.create_packet(nu_frame=42)
        validator.validate_and_parse(pkt)
        self.assertEqual(validator.out_of_order_packets, 0)

        # Duplicate
        validator.validate_and_parse(pkt)
        self.assertEqual(validator.out_of_order_packets, 1)

    def test_late_out_of_order_packet(self):
        """Tier 2: Verify late packet arriving after newer frame is flagged."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        pkt10 = VbanPacketFactory.create_packet(nu_frame=10)
        validator.validate_and_parse(pkt10)

        pkt15 = VbanPacketFactory.create_packet(nu_frame=15)
        validator.validate_and_parse(pkt15)
        self.assertEqual(validator.lost_packets, 4)

        # Packet 12 arrives late (already superseded by 15)
        pkt12 = VbanPacketFactory.create_packet(nu_frame=12)
        validator.validate_and_parse(pkt12)
        self.assertEqual(validator.out_of_order_packets, 1)


if __name__ == "__main__":
    unittest.main()
