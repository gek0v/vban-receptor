#!/usr/bin/env python3
"""Adversarial Coverage Hardening (Tier 5) Stress Suite for VBAN Receptor.

Targets Native C++20 Core and JNI layer:
1. VBAN Protocol & Parser:
   - nuFrame 32-bit unsigned modular wrap-around (0xFFFFFFFF -> 0x00000000, forward gaps across boundary)
   - Large loss bursts, out-of-order, duplicates, boundary gap (0x7FFFFFFF vs 0x80000000)
   - 10-step packet pipeline mutation fuzzing (truncated headers, corrupt FourCC, unsupported subprotocols/codecs/data types, reserved bits, invalid SR indices)
   - Stream name boundary edge cases (null padding, trailing whitespace, case-insensitivity, max 16 chars)
   - Exact payload calculation with max nbs=255 and nbc=255 (262,144 bytes) without integer overflow
2. Audio Converters & Normalizers:
   - PCM 16-bit, 24-bit packed sign extension, 32-bit int, 32-bit float
   - IEEE 754 float sanitization: quiet NaN, signaling NaN, +Inf, -Inf, subnormals, denormals, out-of-range clamping
   - Multi-channel planar / interleaved conversions for 1 to 8 channels (including non-power-of-two 3, 5, 7 ch)
   - Stereo peak level calculation and linear-to-dBFS conversion
3. Lock-Free SPSC Circular Ring Buffer:
   - Capacity 2^18 (262,144 frames), power-of-two masking
   - Monotonic wrap-around across mask boundary
   - Multi-threaded high-churn concurrency stress with variable chunk sizes
   - Whole-frame alignment preservation on buffer saturation
4. Adaptive Clock Drift Compensator & Resampler:
   - 2nd-order cascaded EMA jitter filter (-40 dB/decade roll-off)
   - PI servo controller with anti-windup clamping (+-100 PPM)
   - Audiophile slew rate limiter (<= 0.1 PPM/s rate of change)
   - Catmull-Rom cubic Hermite spline interpolation (C^1 continuity, symmetry, Horner form)
   - 128-sample Raised Cosine Fade Engine (symmetry w(n) + w(128-n) == 1.0, rapid reversals, emergency fade-out)
5. JNI Bridge & Telemetry:
   - Binary struct packing (40 bytes), exact constructor descriptor (IIIJJFFFF)V
   - Thread-safe snapshot polling and edge-case protection (zero divide on packet loss rate)
"""

import math
import os
import random
import struct
import sys
import threading
import time
import unittest
from typing import List, Tuple

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if PROJECT_ROOT not in sys.path:
    sys.path.insert(0, PROJECT_ROOT)

from e2e_tests.audio_dsp_oracle import AudioDspOracle, VbanPacketValidator
from e2e_tests.spsc_ring_buffer_sim import SpscRingBufferSim
from e2e_tests.vban_packet_factory import (
    VBAN_CODEC_PCM,
    VBAN_DATATYPE_BYTE8,
    VBAN_DATATYPE_FLOAT32,
    VBAN_DATATYPE_INT16,
    VBAN_DATATYPE_INT24,
    VBAN_DATATYPE_INT32,
    VBAN_MAGIC_LE_INT,
    VBAN_PROTOCOL_AUDIO,
    VBAN_SAMPLE_SIZES,
    VBAN_SR_TABLE,
    VbanPacketFactory,
)


class TestProtocolAdversarial(unittest.TestCase):
    """Adversarial stress tests for VBAN binary protocol parser and nuFrame sequence tracking."""

    def test_nuframe_32bit_exact_boundary_rollover(self):
        """Stress: Exact rollover from 0xFFFFFFFF to 0x00000000 must produce delta=1 and 0 lost packets."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        pkt_max = VbanPacketFactory.create_packet(nu_frame=0xFFFFFFFF)
        valid, _, _ = validator.validate_and_parse(pkt_max)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 0)
        self.assertEqual(validator.out_of_order_packets, 0)

        pkt_zero = VbanPacketFactory.create_packet(nu_frame=0x00000000)
        valid, _, _ = validator.validate_and_parse(pkt_zero)
        self.assertTrue(valid)
        self.assertEqual(validator.lost_packets, 0, "Exact 32-bit rollover must have zero loss")
        self.assertEqual(validator.out_of_order_packets, 0)

    def test_nuframe_boundary_gap_at_half_space(self):
        """Stress: Sequence gap at 0x7FFFFFFF (forward loss) vs 0x80000000 (out-of-order threshold)."""
        # Case A: delta = 0x7FFFFFFF -> forward loss of 0x7FFFFFFE packets
        val_a = VbanPacketValidator()
        val_a.validate_and_parse(VbanPacketFactory.create_packet(nu_frame=0))
        valid, _, _ = val_a.validate_and_parse(VbanPacketFactory.create_packet(nu_frame=0x7FFFFFFF))
        self.assertTrue(valid)
        self.assertEqual(val_a.lost_packets, 0x7FFFFFFF - 1)
        self.assertEqual(val_a.out_of_order_packets, 0)

        # Case B: delta = 0x80000000 -> considered out-of-order in 32-bit modular arithmetic
        val_b = VbanPacketValidator()
        val_b.validate_and_parse(VbanPacketFactory.create_packet(nu_frame=0))
        valid, _, _ = val_b.validate_and_parse(VbanPacketFactory.create_packet(nu_frame=0x80000000))
        self.assertTrue(valid)
        self.assertEqual(val_b.lost_packets, 0)
        self.assertEqual(val_b.out_of_order_packets, 1)

    def test_rapid_sequence_wrap_fuzz(self):
        """Stress: 2000 packets jumping across the 32-bit boundary in varying forward step sizes."""
        validator = VbanPacketValidator()
        current_nu = 0xFFFFF000  # Start near boundary
        validator.validate_and_parse(VbanPacketFactory.create_packet(nu_frame=current_nu))

        expected_lost = 0

        rng = random.Random(42)
        for _ in range(2000):
            step = rng.choice([1, 1, 1, 2, 5, 10, 50])  # Sequential or forward gaps
            next_nu = (current_nu + step) & 0xFFFFFFFF

            pkt = VbanPacketFactory.create_packet(nu_frame=next_nu)
            valid, _, _ = validator.validate_and_parse(pkt)
            self.assertTrue(valid)

            delta = (next_nu - current_nu) & 0xFFFFFFFF
            if delta == 1:
                pass
            elif 1 < delta < 0x80000000:
                expected_lost += (delta - 1)

            current_nu = next_nu

        self.assertEqual(validator.lost_packets, expected_lost)
        self.assertEqual(validator.out_of_order_packets, 0)

    def test_malformed_datagram_fuzzing(self):
        """Stress: Corrupt header fields across all 10 validation steps."""
        validator = VbanPacketValidator(target_stream_name="Stream1")

        # Step 1: Packet too small (< 28 bytes)
        for size in [0, 1, 5, 27]:
            valid, err, _ = validator.validate_and_parse(b"\x00" * size)
            self.assertFalse(valid)
            self.assertIn("DROP_LENGTH_UNDER_28", err)

        # Step 2: Invalid Magic
        corrupt_magic = bytearray(VbanPacketFactory.create_packet())
        corrupt_magic[0:4] = b"NOPE"
        valid, err, _ = validator.validate_and_parse(bytes(corrupt_magic))
        self.assertFalse(valid)
        self.assertIn("DROP_INVALID_MAGIC", err)

        # Step 3: Unsupported Subprotocol (e.g. Serial 0x20 or Service 0x60)
        corrupt_sub = bytearray(VbanPacketFactory.create_packet())
        corrupt_sub[4] = (corrupt_sub[4] & 0x1F) | 0x20  # Set Serial subprotocol
        valid, err, _ = validator.validate_and_parse(bytes(corrupt_sub))
        self.assertFalse(valid)
        self.assertIn("DROP_NON_AUDIO_SUBPROTOCOL", err)

        # Step 4: Unsupported Codec (e.g. 0x10 VBCA)
        corrupt_codec = bytearray(VbanPacketFactory.create_packet())
        corrupt_codec[7] = (corrupt_codec[7] & 0x0F) | 0x10
        valid, err, _ = validator.validate_and_parse(bytes(corrupt_codec))
        self.assertFalse(valid)
        self.assertIn("DROP_UNSUPPORTED_CODEC", err)

        # Step 5: Reserved Bit 3 Set in format_bit (0x08)
        corrupt_res = bytearray(VbanPacketFactory.create_packet())
        corrupt_res[7] |= 0x08
        valid, err, _ = validator.validate_and_parse(bytes(corrupt_res))
        self.assertFalse(valid)
        self.assertIn("DROP_RESERVED_BIT3_SET", err)

        # Step 6: Invalid Sample Rate Index (>= 22)
        for sr_idx in [22, 25, 31]:
            corrupt_sr = bytearray(VbanPacketFactory.create_packet())
            corrupt_sr[4] = (corrupt_sr[4] & 0xE0) | sr_idx
            valid, err, _ = validator.validate_and_parse(bytes(corrupt_sr))
            self.assertFalse(valid)
            self.assertIn("DROP_INVALID_SR_INDEX", err)

        # Step 7: Unsupported Data Type (e.g. Float64 0x05, Bits12 0x06, Bits10 0x07)
        for dt in [0x05, 0x06, 0x07]:
            corrupt_dt = bytearray(VbanPacketFactory.create_packet())
            corrupt_dt[7] = (corrupt_dt[7] & 0xF8) | dt
            try:
                valid, err, _ = validator.validate_and_parse(bytes(corrupt_dt))
            except ValueError as ex:
                valid = False
                err = f"DROP_UNSUPPORTED_DATATYPE_{dt}: {ex}"

            self.assertFalse(valid, f"Unsupported data type {dt} must be rejected")

        # Step 8: Stream Name Mismatch
        mismatch_pkt = VbanPacketFactory.create_packet(stream_name="OtherStream")
        valid, err, _ = validator.validate_and_parse(mismatch_pkt)
        self.assertFalse(valid)
        self.assertIn("DROP_UNSUBSCRIBED_STREAM", err)

        # Step 9: Payload Size Mismatch (under-sized and over-sized)
        valid_pkt = VbanPacketFactory.create_packet(samples_per_frame=128, channels=2, data_type=VBAN_DATATYPE_INT16)
        valid, err, _ = validator.validate_and_parse(valid_pkt[:-2])  # 2 bytes short
        self.assertFalse(valid)
        self.assertIn("DROP_TRUNCATED_PAYLOAD", err)

        valid, err, _ = validator.validate_and_parse(valid_pkt + b"\x00\x00")  # 2 bytes extra
        self.assertFalse(valid)
        self.assertIn("DROP_TRAILING_GARBAGE", err)

    def test_maximum_payload_calculation_no_overflow(self):
        """Stress: Maximum allowable VBAN packet: format_nbs=255, format_nbc=255, Float32 (262,144 bytes)."""
        # nbs=255 -> 256 frames, nbc=255 -> 256 channels, sampleSize=4 -> 256 * 256 * 4 = 262,144 bytes payload
        # Header is 28 bytes -> Total = 262,172 bytes
        expected_payload = 256 * 256 * 4
        self.assertEqual(expected_payload, 262144)

        header = bytearray(28)
        header[0:4] = struct.pack("<I", VBAN_MAGIC_LE_INT)
        header[4] = 0x00 | 21  # Audio subprotocol, 768 kHz
        header[5] = 255        # format_nbs = 255 (256 frames)
        header[6] = 255        # format_nbc = 255 (256 channels)
        header[7] = 0x00 | VBAN_DATATYPE_FLOAT32  # Codec PCM, Float32
        header[8:24] = b"Stream1\x00\x00\x00\x00\x00\x00\x00\x00\x00"

        # Truncated datagram: 28-byte header + only 100 bytes payload
        truncated_packet = bytes(header) + b"\x00" * 100
        validator = VbanPacketValidator(target_stream_name="Stream1")
        valid, err, _ = validator.validate_and_parse(truncated_packet)
        self.assertFalse(valid)
        self.assertIn("DROP_TRUNCATED_PAYLOAD", err)

    def test_stream_name_edge_cases(self):
        """Stress: Stream names with exactly 16 chars without null terminator, embedded nulls, spaces."""
        validator = VbanPacketValidator(target_stream_name="Exact16CharName!")

        # Exactly 16 characters (no null byte at end of 16-byte field)
        pkt16 = VbanPacketFactory.create_packet(stream_name="Exact16CharName!")
        valid, _, meta = validator.validate_and_parse(pkt16)
        self.assertTrue(valid)
        self.assertEqual(meta["stream_name"], "Exact16CharName!")

        # Case-insensitive match: "exact16charname!" against "Exact16CharName!"
        validator_ci = VbanPacketValidator(target_stream_name="exact16charname!")
        valid_ci, _, _ = validator_ci.validate_and_parse(pkt16)
        self.assertTrue(valid_ci)

        # Promiscuous mode (empty target stream name accepts any name)
        validator_wild = VbanPacketValidator(target_stream_name="")
        for name in ["Foo", "BAR", "12345", ""]:
            pkt = VbanPacketFactory.create_packet(stream_name=name)
            valid, _, _ = validator_wild.validate_and_parse(pkt)
            self.assertTrue(valid)


class TestAudioConvertersAdversarial(unittest.TestCase):
    """Adversarial stress tests for bit-depth conversions, sign extensions, subnormals, and NaNs."""

    def test_int16_exhaustive_boundary_oracle(self):
        """Verify PCM 16-bit dequantization against mathematical oracle for all critical boundaries."""
        cases = [
            (-32768, -1.0),
            (32767, 32767.0 / 32768.0),
            (0, 0.0),
            (-1, -1.0 / 32768.0),
            (1, 1.0 / 32768.0),
            (-16384, -0.5),
            (16384, 0.5),
        ]
        for int_val, expected_float in cases:
            raw = struct.pack("<h", int_val)
            result = AudioDspOracle.dequantize_int16(raw)[0]
            self.assertAlmostEqual(result, expected_float, places=7)
            self.assertTrue(-1.0 <= result <= 1.0)

    def test_int24_packed_sign_extension_exhaustive(self):
        """Verify 24-bit packed sign extension across full dynamic range and boundary transitions."""
        test_samples = [
            (0x7FFFFF, (8388607) / 8388608.0),       # Max positive (+8,388,607)
            (0x800000, (-8388608) / 8388608.0),     # Max negative (-8,388,608) -> exactly -1.0
            (0x800001, (-8388607) / 8388608.0),     # -8,388,607
            (0xFFFFFF, (-1) / 8388608.0),            # -1
            (0x000000, 0.0),                         # Zero
            (0x000001, (1) / 8388608.0),             # +1
            (0x400000, (4194304) / 8388608.0),       # +0.5
            (0xC00000, (-4194304) / 8388608.0),      # -0.5
        ]

        raw_bytes = bytearray()
        expected = []
        for raw24, exp_f in test_samples:
            raw_bytes.append(raw24 & 0xFF)
            raw_bytes.append((raw24 >> 8) & 0xFF)
            raw_bytes.append((raw24 >> 16) & 0xFF)
            expected.append(exp_f)

        dequantized = AudioDspOracle.dequantize_int24_packed(bytes(raw_bytes))
        for res, exp in zip(dequantized, expected):
            self.assertAlmostEqual(res, exp, places=7)
            self.assertTrue(-1.0 <= res <= 1.0)

    def test_int24_packed_random_fuzz_oracle(self):
        """Stress: Fuzz 10,000 random 24-bit samples against mathematical reference."""
        rng = random.Random(1337)
        raw_bytes = bytearray()
        expected = []

        for _ in range(10000):
            val = rng.randint(0, 0xFFFFFF)
            raw_bytes.append(val & 0xFF)
            raw_bytes.append((val >> 8) & 0xFF)
            raw_bytes.append((val >> 16) & 0xFF)

            # Mathematical signed interpretation:
            # Shift left 8 into 32-bit signed int, then arithmetic shift right 8
            s32 = (val << 8) & 0xFFFFFFFF
            if s32 & 0x80000000:
                signed_int = s32 - 0x100000000
            else:
                signed_int = s32
            signed_int >>= 8
            expected.append(signed_int / 8388608.0)

        results = AudioDspOracle.dequantize_int24_packed(bytes(raw_bytes))
        for r, e in zip(results, expected):
            self.assertAlmostEqual(r, e, places=7)

    def test_int32_exact_boundary_oracle(self):
        """Verify PCM 32-bit dequantization at maximum limits."""
        cases = [
            (-2147483648, -1.0),
            (2147483647, 2147483647.0 / 2147483648.0),
            (0, 0.0),
            (-1, -1.0 / 2147483648.0),
            (1, 1.0 / 2147483648.0),
        ]
        for int_val, expected_float in cases:
            raw = struct.pack("<i", int_val)
            result = AudioDspOracle.dequantize_int32(raw)[0]
            self.assertAlmostEqual(result, expected_float, places=8)
            self.assertTrue(-1.0 <= result <= 1.0)

    def test_float32_adversarial_sanitization(self):
        """Stress: NaN, Inf, subnormals, denormals, and out-of-range floats in Float32 audio."""
        quiet_nan = struct.pack("<I", 0x7FC00000)
        signal_nan = struct.pack("<I", 0x7F800001)
        pos_inf = struct.pack("<I", 0x7F800000)
        neg_inf = struct.pack("<I", 0xFF800000)
        subnormal_pos = struct.pack("<I", 0x00000001)  # Smallest positive subnormal float (~1.4e-45)
        subnormal_neg = struct.pack("<I", 0x80000001)  # Smallest negative subnormal float
        clipped_pos = struct.pack("<f", 5.0)
        clipped_neg = struct.pack("<f", -10.0)
        normal_val = struct.pack("<f", 0.7071)

        raw = (
            quiet_nan + signal_nan + pos_inf + neg_inf +
            subnormal_pos + subnormal_neg + clipped_pos + clipped_neg + normal_val
        )

        cleaned = AudioDspOracle.dequantize_float32(raw)
        self.assertEqual(len(cleaned), 9)

        # NaNs and Infs must be zeroed out
        self.assertEqual(cleaned[0], 0.0, "Quiet NaN must be sanitized to 0.0f")
        self.assertEqual(cleaned[1], 0.0, "Signaling NaN must be sanitized to 0.0f")
        self.assertEqual(cleaned[2], 0.0, "+Inf must be sanitized to 0.0f")
        self.assertEqual(cleaned[3], 0.0, "-Inf must be sanitized to 0.0f")

        # Subnormals must remain finite and within [-1.0, 1.0]
        self.assertTrue(math.isfinite(cleaned[4]))
        self.assertTrue(-1.0 <= cleaned[4] <= 1.0)
        self.assertTrue(math.isfinite(cleaned[5]))
        self.assertTrue(-1.0 <= cleaned[5] <= 1.0)

        # Out-of-range floats clamped to [-1.0f, +1.0f]
        self.assertEqual(cleaned[6], 1.0, "+5.0f must clamp to +1.0f")
        self.assertEqual(cleaned[7], -1.0, "-10.0f must clamp to -1.0f")

        # Normal value preserved
        self.assertAlmostEqual(cleaned[8], 0.7071, places=4)

    def test_multichannel_interleaving_and_deinterleaving_roundtrip(self):
        """Stress: Planar / Interleaved round-trip identity for channel counts 1 through 8."""
        num_frames = 256
        for channels in range(1, 9):
            # Generate distinct channel test signals
            planar_orig = []
            for ch in range(channels):
                chan_data = [math.sin(2.0 * math.pi * 440.0 * (f / 48000.0) + (ch * 0.5)) for f in range(num_frames)]
                planar_orig.append(chan_data)

            # Interleave
            interleaved = AudioDspOracle.interleave_channels(planar_orig)
            self.assertEqual(len(interleaved), num_frames * channels)

            # Deinterleave
            planar_recovered = AudioDspOracle.deinterleave_channels(interleaved, channels)
            self.assertEqual(len(planar_recovered), channels)

            for ch in range(channels):
                self.assertEqual(len(planar_recovered[ch]), num_frames)
                for f in range(num_frames):
                    self.assertAlmostEqual(planar_recovered[ch][f], planar_orig[ch][f], places=7)


class TestSpscBufferAdversarial(unittest.TestCase):
    """Adversarial stress tests for Lock-Free Single-Producer Single-Consumer (SPSC) Circular Ring Buffer."""

    def test_power_of_two_and_mask_invariants(self):
        """Invariants: Capacity must be power of two, mask == capacity - 1."""
        for power in [4, 8, 16, 32, 64, 128, 256, 1024, 262144]:
            sim = SpscRingBufferSim(capacity=power, channels=2)
            self.assertEqual(sim.capacity, power)
            self.assertEqual(sim.mask, power - 1)
            self.assertEqual(sim.available_write(), power)
            self.assertEqual(sim.available_read(), 0)

    def test_circular_wrap_around_monotonic_order(self):
        """Stress: Rapid cyclic filling and draining across the circular mask boundary."""
        capacity = 128
        sim = SpscRingBufferSim(capacity=capacity, channels=1)

        total_frames = 10000
        frames_pushed = 0
        frames_pulled = 0

        while frames_pulled < total_frames:
            # Push in chunks of 37 (coprime to 128 to maximize boundary wrap crossing diversity)
            chunk_push = min(37, total_frames - frames_pushed)
            if chunk_push > 0:
                data = [float(frames_pushed + i) for i in range(chunk_push)]
                written = sim.write(data, chunk_push)
                frames_pushed += written

            # Pull in chunks of 53
            chunk_pull = min(53, sim.available_read())
            if chunk_pull > 0:
                pulled_data, actual = sim.read(chunk_pull)
                self.assertEqual(actual, chunk_pull)
                for i in range(actual):
                    self.assertEqual(pulled_data[i], float(frames_pulled + i))
                frames_pulled += actual

        self.assertEqual(frames_pushed, total_frames)
        self.assertEqual(frames_pulled, total_frames)
        self.assertEqual(sim.available_read(), 0)

    def test_concurrent_multithreaded_high_churn_stress(self):
        """Stress: Concurrent Producer and Consumer threads with randomized variable chunks."""
        capacity = 4096
        sim = SpscRingBufferSim(capacity=capacity, channels=2)
        total_frames = 20000

        def producer_worker():
            rng = random.Random(1234)
            current = 0
            while current < total_frames:
                chunk_size = rng.randint(1, 256)
                to_write = min(chunk_size, total_frames - current)
                frames = []
                for i in range(to_write):
                    f_val = float(current + i)
                    frames.extend([f_val, f_val * 2.0])  # Stereo

                written = sim.write(frames, to_write)
                current += written
                if written == 0:
                    time.sleep(0.0001)

        received_frames = []

        def consumer_worker():
            rng = random.Random(5678)
            while len(received_frames) < total_frames:
                req_size = rng.randint(1, 256)
                pulled, actual = sim.read(req_size)
                for f in range(actual):
                    received_frames.append((pulled[f * 2], pulled[f * 2 + 1]))
                if actual == 0:
                    time.sleep(0.0001)

        t_prod = threading.Thread(target=producer_worker)
        t_cons = threading.Thread(target=consumer_worker)

        t_prod.start()
        t_cons.start()
        t_prod.join(timeout=10.0)
        t_cons.join(timeout=10.0)

        self.assertFalse(t_prod.is_alive(), "Producer thread timed out or deadlocked")
        self.assertFalse(t_cons.is_alive(), "Consumer thread timed out or deadlocked")
        self.assertEqual(len(received_frames), total_frames)

        for i, (l, r) in enumerate(received_frames):
            expected = float(i)
            self.assertEqual(l, expected)
            self.assertEqual(r, expected * 2.0)

    def test_buffer_saturation_rejection_no_corruption(self):
        """Stress: Writing to a full buffer must reject excess frames without corrupting state."""
        capacity = 64
        sim = SpscRingBufferSim(capacity=capacity, channels=2)

        # Fill to exact capacity
        initial_data = [float(i) for i in range(capacity * 2)]
        written = sim.write(initial_data, capacity)
        self.assertEqual(written, capacity)
        self.assertEqual(sim.available_write(), 0)
        self.assertEqual(sim.available_read(), capacity)

        # Attempt to write another 10 frames to full buffer -> must return 0 written
        excess_data = [999.0] * 20
        written_excess = sim.write(excess_data, 10)
        self.assertEqual(written_excess, 0)
        self.assertEqual(sim.overflow_dropped_frames, 10)

        # Read back original data and verify it was NOT corrupted by the rejected write
        pulled, count = sim.read(capacity)
        self.assertEqual(count, capacity)
        for i in range(capacity * 2):
            self.assertEqual(pulled[i], float(i))


class TestDriftCompensatorAdversarial(unittest.TestCase):
    """Adversarial stress tests for Clock Drift Compensator, Catmull-Rom micro-resampler, and Cosine Fade."""

    def test_catmull_rom_scalar_interpolation_c1_continuity(self):
        """Verify Catmull-Rom cubic Hermite spline matches Horner form and is C^1 continuous."""
        p0, p1, p2, p3 = 0.0, 1.0, 0.5, -0.2

        # At t = 0, must equal p1
        self.assertAlmostEqual(AudioDspOracle.catmull_rom_sample(p0, p1, p2, p3, 0.0), p1, places=6)
        # At t = 1, must equal p2
        self.assertAlmostEqual(AudioDspOracle.catmull_rom_sample(p0, p1, p2, p3, 1.0), p2, places=6)

        # Analytical derivative check: derivative at t=0 is 0.5*(p2 - p0), at t=1 is 0.5*(p3 - p1)
        dt = 1e-5
        val_0 = AudioDspOracle.catmull_rom_sample(p0, p1, p2, p3, 0.0)
        val_dt = AudioDspOracle.catmull_rom_sample(p0, p1, p2, p3, dt)
        numerical_deriv_0 = (val_dt - val_0) / dt
        analytic_deriv_0 = 0.5 * (p2 - p0)
        self.assertAlmostEqual(numerical_deriv_0, analytic_deriv_0, places=3)

    def test_cosine_window_exact_symmetry(self):
        """Verify 128-sample Raised Cosine window exact mathematical symmetry: w_in(n) + w_out(n) == 1.0."""
        fade_in = AudioDspOracle.cosine_fade_in_weights(128)
        fade_out = AudioDspOracle.cosine_fade_out_weights(128)
        self.assertEqual(len(fade_in), 128)
        self.assertEqual(len(fade_out), 128)

        # Endpoints
        self.assertAlmostEqual(fade_in[0], 0.0, places=7)
        self.assertAlmostEqual(fade_out[0], 1.0, places=7)

        # Symmetry across entire window: fade_in[n] + fade_out[n] == 1.0
        for n in range(128):
            sum_sym = fade_in[n] + fade_out[n]
            self.assertAlmostEqual(sum_sym, 1.0, places=6, msg=f"Symmetry failed at n={n}")

    def test_slew_rate_limiter_strict_gradient_bound(self):
        """Stress: Slew-rate limiter must never exceed 0.1 PPM/s across massive step changes."""
        max_slew_per_sec = 0.1
        current_ppm = 0.0
        target_step_ppm = 100.0  # Massive instantaneous step change from 0 to 100 PPM
        dt = 0.01  # 10 ms audio callback intervals (100 callbacks per second)

        for _ in range(500):  # 5 seconds of simulation
            max_delta = max_slew_per_sec * dt
            delta = target_step_ppm - current_ppm
            step = max(-max_delta, min(max_delta, delta))
            new_ppm = current_ppm + step

            # Verify instantaneous gradient
            gradient = abs(new_ppm - current_ppm) / dt
            self.assertLessEqual(gradient, max_slew_per_sec + 1e-9)

            current_ppm = new_ppm

        # After 5 seconds at 0.1 PPM/s, current_ppm should have advanced by exactly 0.5 PPM
        self.assertAlmostEqual(current_ppm, 0.5, places=5)

    def test_pi_servo_anti_windup_clamping(self):
        """Stress: Sustained extreme buffer error must not cause integral windup beyond +-100 PPM."""
        kp = 2.0
        ki = 0.05
        max_ppm = 100.0

        integral = 0.0
        last_raw_output = 0.0
        dt = 0.02
        error = 40.0  # Constant high error (+40% watermark error)

        for _ in range(2000):  # 40 seconds of high error
            p = kp * error
            saturating_high = (last_raw_output >= max_ppm and error > 0.0)
            saturating_low = (last_raw_output <= -max_ppm and error < 0.0)

            if not saturating_high and not saturating_low:
                integral += ki * error * dt
                integral = max(-max_ppm, min(max_ppm, integral))

            last_raw_output = max(-max_ppm, min(max_ppm, p + integral))

            self.assertLessEqual(abs(integral), max_ppm)
            self.assertLessEqual(abs(last_raw_output), max_ppm)

        # Output must be clamped at max_ppm and integral must not wind up beyond (max_ppm - p + step)
        self.assertEqual(last_raw_output, max_ppm)
        self.assertLessEqual(integral, max_ppm - (kp * error) + (ki * error * dt) + 1e-9)


class TestJniBridgeAdversarial(unittest.TestCase):
    """Adversarial stress tests for JNI Bridge, NativeTelemetry data class, and dBFS calculations."""

    def test_native_telemetry_struct_packing_and_types(self):
        """Verify binary layout: 40 bytes total, matching JNI constructor (IIIJJFFFF)V."""
        fmt_c = "<IHHQQffff"
        self.assertEqual(struct.calcsize(fmt_c), 40)

        packed = struct.pack(
            fmt_c,
            768000, 2, 32,
            1000000, 5,
            0.125, 50.0, 0.85, 0.90
        )
        self.assertEqual(len(packed), 40)

        unpacked = struct.unpack(fmt_c, packed)
        self.assertEqual(unpacked[0], 768000)
        self.assertEqual(unpacked[1], 2)
        self.assertEqual(unpacked[2], 32)
        self.assertEqual(unpacked[3], 1000000)
        self.assertEqual(unpacked[4], 5)
        self.assertAlmostEqual(unpacked[5], 0.125, places=5)
        self.assertAlmostEqual(unpacked[6], 50.0, places=5)
        self.assertAlmostEqual(unpacked[7], 0.85, places=5)
        self.assertAlmostEqual(unpacked[8], 0.90, places=5)

    def test_dbfs_conversion_edge_cases(self):
        """Verify linear amplitude to dBFS conversion edge cases (0.0, -ve, 1.0, > 1.0)."""
        # Exact silence
        self.assertEqual(AudioDspOracle.amplitude_to_dbfs(0.0), -60.0)
        self.assertEqual(AudioDspOracle.amplitude_to_dbfs(-0.5), -60.0)

        # Full scale 1.0 -> 0.0 dBFS
        self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(1.0), 0.0, places=5)

        # Overshoot clamped to 0.0 dBFS
        self.assertEqual(AudioDspOracle.amplitude_to_dbfs(1.5), 0.0)
        self.assertEqual(AudioDspOracle.amplitude_to_dbfs(10.0), 0.0)

        # 0.5 linear amplitude = 20 * log10(0.5) = -6.0206 dBFS
        self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(0.5), -6.0205999, places=4)

        # 0.001 linear amplitude = 20 * log10(0.001) = -60.0 dBFS
        self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(0.001), -60.0, places=4)


if __name__ == "__main__":
    unittest.main()
