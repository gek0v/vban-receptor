"""Tier 4: Real-World Application Scenarios Test Suite.

Executes realistic end-to-end streaming workloads:
- Scenario 1: Ultra-Audiophile 768 kHz 32-bit Float Stereo Streaming (1,000 packets, 256,000 frames)
- Scenario 2: Red Book Audio CD 44.1 kHz 16-bit PCM Streaming (1 second of CD audio)
- Scenario 3: Studio Master 192 kHz 24-bit Packed Hi-Res Streaming (Dynamic range > 120 dB)
- Scenario 4: Bursty Wi-Fi Jitter Burst Recovery (120 ms sleep burst absorption & recovery)
- Scenario 5: Multi-Room Multicast Reception (Stream isolation among multiple streams on 239.255.0.1)
"""

import math
import unittest
from e2e_tests.audio_dsp_oracle import AudioDspOracle, VbanPacketValidator
from e2e_tests.spsc_ring_buffer_sim import SpscRingBufferSim
from e2e_tests.vban_packet_factory import (
    VbanPacketFactory,
    VBAN_DATATYPE_FLOAT32,
    VBAN_DATATYPE_INT16,
    VBAN_DATATYPE_INT24,
)


class TestRealWorldWorkloads(unittest.TestCase):
    """Tier 4 realistic application scenario tests."""

    def test_scenario_1_audiophile_768k_streaming(self):
        """Scenario 1: 768 kHz 32-bit float stereo streaming (1,000 packets, 256,000 frames at ~6.14 MB/s)."""
        sr = 768000
        samples_per_pkt = 256
        channels = 2
        total_packets = 1000  # 256,000 frames (~333.3 ms of ultra-high-resolution audio)
        freq = 1000.0  # 1 kHz test sine

        validator = VbanPacketValidator(target_stream_name="Audiophile768k")
        buf = SpscRingBufferSim(capacity=262144, channels=2)

        # Producer & consumer simulation
        received_samples = []

        for p in range(total_packets):
            # Synthesize 1 kHz sine for Left, inverted sine for Right
            pkt_samples = []
            for s in range(samples_per_pkt):
                t = (p * samples_per_pkt + s) / sr
                left = math.sin(2.0 * math.pi * freq * t)
                right = -left
                pkt_samples.extend([left, right])

            datagram = VbanPacketFactory.create_packet(
                sample_rate=sr,
                samples_per_frame=samples_per_pkt,
                channels=channels,
                data_type=VBAN_DATATYPE_FLOAT32,
                stream_name="Audiophile768k",
                nu_frame=p,
                samples_data=pkt_samples,
            )

            # Ingest datagram
            valid, reason, info = validator.validate_and_parse(datagram)
            self.assertTrue(valid, f"Packet {p} failed validation: {reason}")
            self.assertEqual(info["sample_rate"], 768000)

            # Push to SPSC ring buffer
            written = buf.write(info["samples"], samples_per_pkt)
            self.assertEqual(written, samples_per_pkt)

            # Consumer pulls 256 frames on each audio cycle
            pulled_frames, actual = buf.read(samples_per_pkt)
            self.assertEqual(actual, samples_per_pkt)
            received_samples.extend(pulled_frames)

        self.assertEqual(len(received_samples), total_packets * samples_per_pkt * channels)
        self.assertEqual(validator.lost_packets, 0)
        self.assertEqual(buf.overflow_dropped_frames, 0)
        self.assertEqual(buf.underrun_count, 0)

        # Validate RMS of received 1 kHz sine wave: should be 1 / sqrt(2) approx 0.7071 (-3.01 dBFS)
        left_channel = received_samples[0::2]
        rms_left, peak_left = AudioDspOracle.calculate_rms_and_peak(left_channel)
        self.assertAlmostEqual(rms_left, 1.0 / math.sqrt(2.0), places=2)
        self.assertAlmostEqual(peak_left, 1.0, places=2)

    def test_scenario_2_redbook_cd_audio_streaming(self):
        """Scenario 2: Red Book Audio CD 44.1 kHz 16-bit PCM streaming (1 full second = 172 packets)."""
        sr = 44100
        samples_per_pkt = 256
        channels = 2
        total_packets = 172  # 172 * 256 = 44,032 frames (~1 sec)
        freq = 440.0  # 440 Hz concert pitch A

        validator = VbanPacketValidator(target_stream_name="CD_Audio")
        buf = SpscRingBufferSim(capacity=16384, channels=2)

        total_frames_streamed = 0
        for p in range(total_packets):
            pkt_samples = []
            for s in range(samples_per_pkt):
                t = (p * samples_per_pkt + s) / sr
                val = 0.8 * math.sin(2.0 * math.pi * freq * t)
                pkt_samples.extend([val, val])

            datagram = VbanPacketFactory.create_packet(
                sample_rate=sr,
                samples_per_frame=samples_per_pkt,
                channels=channels,
                data_type=VBAN_DATATYPE_INT16,
                stream_name="CD_Audio",
                nu_frame=p,
                samples_data=pkt_samples,
            )

            valid, reason, info = validator.validate_and_parse(datagram)
            self.assertTrue(valid)
            self.assertEqual(info["sample_rate"], 44100)

            buf.write(info["samples"], samples_per_pkt)
            out, actual = buf.read(samples_per_pkt)
            total_frames_streamed += actual

        self.assertEqual(total_frames_streamed, total_packets * samples_per_pkt)
        self.assertEqual(validator.lost_packets, 0)

    def test_scenario_3_studio_master_192k_24bit_streaming(self):
        """Scenario 3: Studio Master 192 kHz 24-bit packed PCM streaming with dynamic range verification."""
        sr = 192000
        samples_per_pkt = 128
        channels = 2
        total_packets = 375  # 48,000 frames (250 ms at 192 kHz)

        validator = VbanPacketValidator(target_stream_name="StudioMaster")
        buf = SpscRingBufferSim(capacity=32768, channels=2)

        # Test low-level audiophile signal: -60 dBFS (amplitude = 0.001)
        low_amp = 0.001
        for p in range(total_packets):
            pkt_samples = []
            for s in range(samples_per_pkt):
                t = (p * samples_per_pkt + s) / sr
                val = low_amp * math.sin(2.0 * math.pi * 1000.0 * t)
                pkt_samples.extend([val, val])

            datagram = VbanPacketFactory.create_packet(
                sample_rate=sr,
                samples_per_frame=samples_per_pkt,
                channels=channels,
                data_type=VBAN_DATATYPE_INT24,
                stream_name="StudioMaster",
                nu_frame=p,
                samples_data=pkt_samples,
            )

            valid, _, info = validator.validate_and_parse(datagram)
            self.assertTrue(valid)
            buf.write(info["samples"], samples_per_pkt)
            out, actual = buf.read(samples_per_pkt)

            # Verify low-level signal was not zeroed out by quantization truncation
            rms, peak = AudioDspOracle.calculate_rms_and_peak(out)
            self.assertGreater(rms, 0.0005)
            self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(peak), -60.0, places=1)

    def test_scenario_4_wifi_jitter_burst_recovery(self):
        """Scenario 4: Simulation of 120 ms Wi-Fi power-save sleep burst, socket absorption and recovery."""
        sr = 48000
        samples_per_pkt = 128  # 128 / 48000 = 2.666 ms per packet
        channels = 2

        validator = VbanPacketValidator(target_stream_name="Stream1")
        # Capacity = 32,768 frames (682 ms at 48 kHz)
        buf = SpscRingBufferSim(capacity=32768, channels=2)

        # 1. Steady state: seed buffer to 50% target watermark (16,384 frames)
        seed_data = [0.0] * (16384 * 2)
        buf.write(seed_data, 16384)
        self.assertEqual(buf.occupancy_percent(), 50.0)

        # 2. Wi-Fi chip enters 120 ms power-save sleep
        # During 120 ms, ~45 packets arrive at network stack and are queued in 4 MB socket buffer.
        # Meanwhile, real-time audio thread consumes 120 ms = 0.12 * 48000 = 5,760 frames.
        burst_packets_count = 45
        audio_frames_drained = 5760
        out, pulled = buf.read(audio_frames_drained)
        self.assertEqual(pulled, 5760)

        # Buffer dropped from 16,384 to 10,624 frames (~32.4% occupancy, stable green/yellow)
        self.assertAlmostEqual(buf.occupancy_percent(), 10624.0 / 32768.0 * 100.0, places=2)
        # Underrun did NOT occur!
        self.assertEqual(buf.underrun_count, 0)

        # 3. Wi-Fi wakes up and delivers the 45 back-to-back packets in an instantaneous burst
        for p in range(burst_packets_count):
            pkt = VbanPacketFactory.create_packet(
                sample_rate=sr, samples_per_frame=samples_per_pkt, nu_frame=p
            )
            valid, _, info = validator.validate_and_parse(pkt)
            self.assertTrue(valid)
            written = buf.write(info["samples"], samples_per_pkt)
            self.assertEqual(written, samples_per_pkt)

        # Buffer refilled by 45 * 128 = 5,760 frames back to exactly 16,384 frames (50% target watermark)
        self.assertEqual(buf.available_read(), 16384)
        self.assertEqual(buf.occupancy_percent(), 50.0)
        self.assertEqual(buf.overflow_dropped_frames, 0)

    def test_scenario_5_multicast_multiroom_reception(self):
        """Scenario 5: Multi-room multicast streaming with 3 concurrent streams on 239.255.0.1."""
        # Receiver is configured for "LivingRoom"
        validator = VbanPacketValidator(target_stream_name="LivingRoom")

        streams = ["LivingRoom", "Bedroom", "Patio"]
        packets_sent = {"LivingRoom": 0, "Bedroom": 0, "Patio": 0}
        accepted_livingroom_packets = 0

        # Simulate 300 interleaved network packets arriving from all 3 rooms
        for i in range(300):
            st_name = streams[i % 3]
            frame_no = packets_sent[st_name]
            packets_sent[st_name] += 1

            pkt = VbanPacketFactory.create_packet(
                stream_name=st_name,
                nu_frame=frame_no,
                samples_per_frame=128,
            )

            valid, reason, info = validator.validate_and_parse(pkt)
            if st_name == "LivingRoom":
                self.assertTrue(valid)
                self.assertEqual(info["stream_name"], "LivingRoom")
                accepted_livingroom_packets += 1
            else:
                self.assertFalse(valid)
                self.assertTrue(reason.startswith("DROP_UNSUBSCRIBED_STREAM"))

        self.assertEqual(accepted_livingroom_packets, 100)
        self.assertEqual(validator.lost_packets, 0)


if __name__ == "__main__":
    unittest.main()
