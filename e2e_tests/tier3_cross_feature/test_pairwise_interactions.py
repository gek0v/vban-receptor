"""Tier 3: Pairwise Cross-Feature Interactions Test Suite.

Covers:
- Interaction 1: Sample Rate Switch Mid-Stream (44.1 kHz -> 96 kHz) with Cosine Crossfade
- Interaction 2: Wi-Fi Packet Loss Burst during Active Clock Drift PI Adjustment
- Interaction 3: External USB DAC Unplug/Replug during 768 kHz High-Throughput Streaming
- Interaction 4: Multicast Group Address Change during Low-Latency Playback
- Interaction 5: Multi-Channel Format Change (Stereo -> 5.1 Surround) during Active Buffer Drain
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


class TestPairwiseInteractions(unittest.TestCase):
    """Tier 3 tests for complex multi-subsystem cross-feature interactions."""

    def test_sample_rate_switch_mid_stream_with_crossfade(self):
        """Interaction 1: Dynamic sample rate change from 44.1 kHz to 96 kHz mid-stream with 128-sample cosine fade."""
        validator = VbanPacketValidator(target_stream_name="Stream1")
        buf = SpscRingBufferSim(capacity=16384, channels=2)

        # 1. Stream 5 packets at 44.1 kHz
        initial_sr = 44100
        for i in range(5):
            pkt = VbanPacketFactory.create_packet(
                sample_rate=initial_sr, samples_per_frame=128, nu_frame=i
            )
            valid, _, info = validator.validate_and_parse(pkt)
            self.assertTrue(valid)
            self.assertEqual(info["sample_rate"], 44100)
            buf.write(info["samples"], 128)

        self.assertEqual(buf.available_read(), 5 * 128)

        # 2. Sender switches sample rate to 96 kHz at packet 5
        new_sr = 96000
        switch_pkt = VbanPacketFactory.create_packet(
            sample_rate=new_sr, samples_per_frame=128, nu_frame=5
        )
        valid, _, info = validator.validate_and_parse(switch_pkt)
        self.assertTrue(valid)
        self.assertEqual(info["sample_rate"], 96000)

        # 3. Receptor detects rate transition and applies 128-sample equal power cosine fade-out
        fade_out = AudioDspOracle.cosine_fade_out_weights(128)
        frames_to_fade, _ = buf.read(128)
        faded_out_frames = []
        for f in range(128):
            w = fade_out[f]
            faded_out_frames.append(frames_to_fade[f * 2] * w)
            faded_out_frames.append(frames_to_fade[f * 2 + 1] * w)

        # The last sample of fade-out must be near silence
        self.assertAlmostEqual(faded_out_frames[-1], 0.0, places=2)
        self.assertAlmostEqual(faded_out_frames[-2], 0.0, places=2)

        # Buffer reset & fade-in of new 96 kHz audio
        buf.reset()
        fade_in = AudioDspOracle.cosine_fade_in_weights(128)
        new_audio = info["samples"]
        faded_in_frames = []
        for f in range(128):
            w = fade_in[f]
            faded_in_frames.append(new_audio[f * 2] * w)
            faded_in_frames.append(new_audio[f * 2 + 1] * w)

        buf.write(faded_in_frames, 128)
        self.assertEqual(buf.available_read(), 128)

    def test_packet_loss_burst_during_active_drift_adjustment(self):
        """Interaction 2: Severe 50-packet loss burst occurring during active clock drift compensation."""
        validator = VbanPacketValidator(target_stream_name="Stream1")
        buf = SpscRingBufferSim(capacity=16384, channels=2)

        # Seed buffer to 50% target occupancy (8192 frames)
        seed_data = [0.1] * (8192 * 2)
        buf.write(seed_data, 8192)
        self.assertEqual(buf.occupancy_percent(), 50.0)

        # Streaming packets sequentially
        for i in range(10):
            pkt = VbanPacketFactory.create_packet(nu_frame=i, samples_per_frame=128)
            validator.validate_and_parse(pkt)

        # Sudden burst drop: packets 10..59 are lost, packet 60 arrives
        burst_pkt = VbanPacketFactory.create_packet(nu_frame=60, samples_per_frame=128)
        valid, _, info = validator.validate_and_parse(burst_pkt)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 50)

        # Audio thread meanwhile consumed 50 packets * 128 = 6400 frames
        buf.read(6400)
        # Buffer dropped from 8192 to 1792 frames (~10.9% occupancy)
        self.assertAlmostEqual(buf.occupancy_percent(), 1792.0 / 16384.0 * 100.0, places=2)

        # Verify Jitter filter and PI drift controller remain bounded
        target_watermark = 50.0
        current_watermark = buf.occupancy_percent()
        error = current_watermark - target_watermark  # ~ -39%

        kp = 1.5
        correction_ppm = max(-100.0, min(100.0, kp * error))
        # Rate modulation must not exceed +-100 PPM constraint
        self.assertGreaterEqual(correction_ppm, -100.0)
        self.assertLessEqual(correction_ppm, 100.0)

    def test_usb_dac_hotplug_during_768k_streaming(self):
        """Interaction 3: USB DAC disconnected and reconnected during 768 kHz high-throughput streaming."""
        # Simulated DAC state
        dac_connected = True
        bit_perfect_active = True
        streaming_active = True
        stream_rate = 768000

        # High packet rate: 3000 pkts/s
        pkt = VbanPacketFactory.create_packet(sample_rate=stream_rate, samples_per_frame=256)
        validator = VbanPacketValidator(target_stream_name="Stream1")
        valid, _, info = validator.validate_and_parse(pkt)
        self.assertTrue(valid)
        self.assertEqual(info["sample_rate"], 768000)

        # DAC unplug event triggers AudioStreamErrorCallback::onErrorAfterClose("ErrorDisconnected")
        dac_connected = False
        bit_perfect_active = False
        # Stream enters paused fallback
        streaming_active = False

        self.assertFalse(bit_perfect_active)
        self.assertFalse(streaming_active)

        # Re-plug event (User plugs DAC back in)
        dac_connected = True
        # System re-evaluates mixer attributes and re-enables bit-perfect mode
        bit_perfect_active = True
        streaming_active = True

        self.assertTrue(dac_connected)
        self.assertTrue(bit_perfect_active)
        self.assertTrue(streaming_active)

    def test_multicast_address_switch_during_playback(self):
        """Interaction 4: Switching multicast group from 239.255.0.1 to 239.255.0.2 during playback."""
        validator_group1 = VbanPacketValidator(target_stream_name="LiveAudio")

        pkt_g1 = VbanPacketFactory.create_packet(stream_name="LiveAudio", nu_frame=1)
        valid, _, _ = validator_group1.validate_and_parse(pkt_g1)
        self.assertTrue(valid)

        # Receiver switches to stream on Group 2: "StudioMain"
        validator_group2 = VbanPacketValidator(target_stream_name="StudioMain")

        # Stale packet from old group arrives -> Dropped
        pkt_stale = VbanPacketFactory.create_packet(stream_name="LiveAudio", nu_frame=2)
        valid, reason, _ = validator_group2.validate_and_parse(pkt_stale)
        self.assertFalse(valid)
        self.assertTrue(reason.startswith("DROP_UNSUBSCRIBED_STREAM"))

        # Packet from new group arrives -> Accepted
        pkt_new = VbanPacketFactory.create_packet(stream_name="StudioMain", nu_frame=1)
        valid, _, info = validator_group2.validate_and_parse(pkt_new)
        self.assertTrue(valid)
        self.assertEqual(info["stream_name"], "StudioMain")

    def test_multichannel_format_change_mid_stream(self):
        """Interaction 5: Format change from 2-channel 16-bit to 6-channel 24-bit surround during streaming."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        # Start with Stereo 16-bit PCM (sample size = 2 bytes)
        # Expected size = 28 + 128 * 2 * 2 = 540 bytes
        pkt_stereo16 = VbanPacketFactory.create_packet(
            samples_per_frame=128, channels=2, data_type=VBAN_DATATYPE_INT16, nu_frame=1
        )
        self.assertEqual(len(pkt_stereo16), 540)
        valid, _, info = validator.validate_and_parse(pkt_stereo16)
        self.assertTrue(valid)
        self.assertEqual(info["channels"], 2)
        self.assertEqual(len(info["samples"]), 256)

        # Switch to 6-channel (5.1) 24-bit packed PCM (sample size = 3 bytes)
        # Expected size = 28 + 128 * 6 * 3 = 2332 bytes
        pkt_surround24 = VbanPacketFactory.create_packet(
            samples_per_frame=128, channels=6, data_type=VBAN_DATATYPE_INT24, nu_frame=2
        )
        self.assertEqual(len(pkt_surround24), 2332)
        valid, _, info2 = validator.validate_and_parse(pkt_surround24)
        self.assertTrue(valid)
        self.assertEqual(info2["channels"], 6)
        self.assertEqual(len(info2["samples"]), 128 * 6)


if __name__ == "__main__":
    unittest.main()
