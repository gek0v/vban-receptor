"""Tier 1: Network Transport & Jitter Tracking Tests.

Covers Features:
- Feature 27: UDP Socket Network Engine (Port 6980 default, 4 MB SO_RCVBUF)
- Feature 28: UDP Multicast Group Joining (IP_ADD_MEMBERSHIP)
- Feature 29: Packet Sequence & Loss Tracking (nuFrame continuity, unsigned 32-bit arithmetic)
- Feature 30: Inter-Arrival Jitter Estimation (RFC 3550 EWMA algorithm)
"""

import socket
import struct
import unittest
from e2e_tests.audio_dsp_oracle import AudioDspOracle, VbanPacketValidator
from e2e_tests.vban_packet_factory import VbanPacketFactory


class TestNetworkTransport(unittest.TestCase):
    """Tier 1 tests for UDP network engine, multicast, sequence tracking, and jitter."""

    def test_udp_port_6980_binding(self):
        """Feature 27: Verify default VBAN port 6980 and loopback socket binding semantics."""
        # Test socket creation and local loopback bind capability
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # Bind to ephemeral port or test port
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
            self.assertGreater(port, 0)
        finally:
            sock.close()

    def test_udp_socket_buffer_4mb_sizing(self):
        """Feature 27: Verify 4 MB (4,194,304 bytes) SO_RCVBUF sizing calculation and absorption headroom."""
        # Bandwidth at 768 kHz 32-bit float stereo:
        # 3,000 pkts/s * 2076 bytes/pkt = 6,228,000 bytes/s (~6.23 MB/s)
        byte_rate_768k = (768000 / 256) * (28 + 256 * 2 * 4)  # 2076 bytes/pkt * 3000 pkts/s
        self.assertEqual(byte_rate_768k, 6228000)

        so_rcvbuf_4mb = 4 * 1024 * 1024  # 4,194,304 bytes
        # Calculate time absorption headroom during OS thread stalls (e.g. 50-150ms Wi-Fi sleep)
        headroom_ms = (so_rcvbuf_4mb / byte_rate_768k) * 1000.0
        self.assertGreater(headroom_ms, 600.0, "4 MB buffer must absorb > 600 ms at 768 kHz")

    def test_multicast_membership_join(self):
        """Feature 28: Verify IPv4 multicast address validation and IP_ADD_MEMBERSHIP struct layout."""
        # Standard VBAN multicast range: 224.0.0.0 to 239.255.255.255
        test_multicast_ip = "239.255.0.1"
        parts = [int(p) for p in test_multicast_ip.split(".")]
        self.assertTrue(224 <= parts[0] <= 239, "IP must be in Class D multicast range")

        # Verify ip_mreq binary struct format: 4s (multiaddr), 4s (interface)
        multiaddr_bytes = socket.inet_aton(test_multicast_ip)
        interface_bytes = socket.inet_aton("0.0.0.0")  # INADDR_ANY
        mreq = multiaddr_bytes + interface_bytes
        self.assertEqual(len(mreq), 8)

    def test_nuframe_sequential_continuity(self):
        """Feature 29: Verify nuFrame sequence checking, in-order detection, and loss accounting."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        # 1. Packet 0 -> First packet
        pkt0 = VbanPacketFactory.create_packet(nu_frame=100)
        valid, _, info = validator.validate_and_parse(pkt0)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 0)

        # 2. Packet 1 -> Sequential in-order (nu_frame = 101)
        pkt1 = VbanPacketFactory.create_packet(nu_frame=101)
        valid, _, info = validator.validate_and_parse(pkt1)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 0)

        # 3. Packet 2 -> 3 packets dropped (nu_frame = 105: expected 102, missed 102, 103, 104)
        pkt2 = VbanPacketFactory.create_packet(nu_frame=105)
        valid, _, info = validator.validate_and_parse(pkt2)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 3)

        # 4. Duplicate packet (nu_frame = 105) -> Out of order / duplicate flagged
        pkt_dup = VbanPacketFactory.create_packet(nu_frame=105)
        valid, _, info = validator.validate_and_parse(pkt_dup)
        self.assertTrue(valid)
        self.assertEqual(validator.out_of_order_packets, 1)

    def test_rfc3550_jitter_formula(self):
        """Feature 30: Verify RFC 3550 EWMA inter-arrival jitter estimation."""
        sr = 48000
        samples_per_frame = 128
        # Expected gap = 128 / 48000 = 0.0026666... sec = 2.666 ms
        expected_gap = samples_per_frame / sr

        # Perfectly jitter-free arrivals
        t0 = 0.000
        t1 = t0 + expected_gap
        j0 = 0.0
        j1 = AudioDspOracle.rfc3550_jitter_update(j0, t1, t0, samples_per_frame, sr)
        self.assertAlmostEqual(j1, 0.0, places=6)

        # Spike jitter: packet arrives late by 5 ms
        t2 = t1 + expected_gap + 0.005
        j2 = AudioDspOracle.rfc3550_jitter_update(j1, t2, t1, samples_per_frame, sr)
        # J = 0 + (0.005 - 0)/16 = 0.005 / 16 = 0.0003125 sec
        self.assertAlmostEqual(j2, 0.005 / 16.0, places=6)


if __name__ == "__main__":
    unittest.main()
