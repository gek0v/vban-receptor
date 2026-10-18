"""Authoritative Audio DSP & Protocol Reference Oracle for VBAN.

Contains exact mathematical models and standard-derived algorithms for:
- PCM Dequantization (16-bit, 24-bit packed, 32-bit int, 32-bit float)
- Channel Interleaving / Deinterleaving
- Catmull-Rom Cubic Hermite Micro-Resampling
- 128-sample Cosine Crossfade Reconfiguration
- RFC 3550 Inter-Arrival Jitter Estimation
- VU Meter dBFS & Peak Decay
- 10-Step VBAN Packet Ingestion Pipeline
"""

import math
import struct
from typing import Dict, List, Optional, Tuple, Union
from .vban_packet_factory import (
    VBAN_SR_TABLE,
    VBAN_PROTOCOL_AUDIO,
    VBAN_CODEC_PCM,
    VBAN_DATATYPE_INT16,
    VBAN_DATATYPE_INT24,
    VBAN_DATATYPE_INT32,
    VBAN_DATATYPE_FLOAT32,
    VBAN_DATATYPE_BYTE8,
    VBAN_SAMPLE_SIZES,
)


class AudioDspOracle:
    """Mathematical reference implementation for audio dequantization and DSP."""

    # Normalization scaling constants
    SCALE_INT16 = 1.0 / 32768.0
    SCALE_INT24 = 1.0 / 8388608.0
    SCALE_INT32 = 1.0 / 2147483648.0

    @classmethod
    def dequantize_int16(cls, raw_bytes: bytes) -> List[float]:
        """Dequantizes 16-bit signed LE PCM to Float32 [-1.0f, +1.0f]."""
        num_samples = len(raw_bytes) // 2
        ints = struct.unpack(f"<{num_samples}h", raw_bytes[: num_samples * 2])
        return [float(i) * cls.SCALE_INT16 for i in ints]

    @classmethod
    def dequantize_int24_packed(cls, raw_bytes: bytes) -> List[float]:
        """Dequantizes 3-byte packed signed LE PCM with sign extension to Float32."""
        num_samples = len(raw_bytes) // 3
        result = []
        for i in range(num_samples):
            idx = i * 3
            b0 = raw_bytes[idx]
            b1 = raw_bytes[idx + 1]
            b2 = raw_bytes[idx + 2]
            # Sign extend 24-bit to 32-bit signed int
            unsigned_val = b0 | (b1 << 8) | (b2 << 16)
            if unsigned_val & 0x800000:
                signed_val = unsigned_val - 0x1000000
            else:
                signed_val = unsigned_val
            result.append(float(signed_val) * cls.SCALE_INT24)
        return result

    @classmethod
    def dequantize_int32(cls, raw_bytes: bytes) -> List[float]:
        """Dequantizes 32-bit signed LE PCM to Float32 [-1.0f, +1.0f]."""
        num_samples = len(raw_bytes) // 4
        ints = struct.unpack(f"<{num_samples}i", raw_bytes[: num_samples * 4])
        return [float(i) * cls.SCALE_INT32 for i in ints]

    @classmethod
    def dequantize_float32(cls, raw_bytes: bytes) -> List[float]:
        """Dequantizes IEEE-754 32-bit float with NaN/Inf sanitization and clamping [-1.0f, +1.0f]."""
        num_samples = len(raw_bytes) // 4
        floats = struct.unpack(f"<{num_samples}f", raw_bytes[: num_samples * 4])
        cleaned = []
        for f in floats:
            if math.isnan(f) or math.isinf(f):
                cleaned.append(0.0)
            else:
                cleaned.append(max(-1.0, min(1.0, float(f))))
        return cleaned

    @classmethod
    def dequantize_payload(cls, raw_payload: bytes, data_type: int) -> List[float]:
        """Dequantizes audio payload according to format bitmask."""
        if data_type == VBAN_DATATYPE_INT16:
            return cls.dequantize_int16(raw_payload)
        elif data_type == VBAN_DATATYPE_INT24:
            return cls.dequantize_int24_packed(raw_payload)
        elif data_type == VBAN_DATATYPE_INT32:
            return cls.dequantize_int32(raw_payload)
        elif data_type == VBAN_DATATYPE_FLOAT32:
            return cls.dequantize_float32(raw_payload)
        elif data_type == VBAN_DATATYPE_BYTE8:
            return [(float(b) - 128.0) / 128.0 for b in raw_payload]
        else:
            raise ValueError(f"Unsupported data_type: {data_type}")

    @staticmethod
    def deinterleave_channels(interleaved_samples: List[float], channels: int) -> List[List[float]]:
        """Splits interleaved frame stream into independent channel arrays."""
        if channels <= 0:
            raise ValueError("channels must be >= 1")
        channel_data = [[] for _ in range(channels)]
        for i, s in enumerate(interleaved_samples):
            channel_data[i % channels].append(s)
        return channel_data

    @staticmethod
    def interleave_channels(channel_data: List[List[float]]) -> List[float]:
        """Interleaves multiple channel arrays into a single interleaved frame stream."""
        if not channel_data:
            return []
        num_channels = len(channel_data)
        num_frames = len(channel_data[0])
        result = []
        for frame_idx in range(num_frames):
            for ch_idx in range(num_channels):
                result.append(channel_data[ch_idx][frame_idx])
        return result

    @staticmethod
    def catmull_rom_sample(y_m1: float, y_0: float, y_1: float, y_2: float, x: float) -> float:
        """Evaluates 4-point Catmull-Rom cubic Hermite spline at fractional offset x in [0.0, 1.0]."""
        c0 = y_0
        c1 = 0.5 * (y_1 - y_m1)
        c2 = y_m1 - 2.5 * y_0 + 2.0 * y_1 - 0.5 * y_2
        c3 = 0.5 * (y_2 - y_m1) + 1.5 * (y_0 - y_1)
        return ((c3 * x + c2) * x + c1) * x + c0

    @classmethod
    def catmull_rom_resample(cls, input_samples: List[float], drift_ratio: float) -> List[float]:
        """Resamples single-channel stream with Catmull-Rom micro-interpolation.
        
        drift_ratio = 1.0 + delta_r (e.g. 1.00005 for +50 PPM).
        """
        if len(input_samples) < 4:
            return list(input_samples)

        output = []
        phase = 0.0
        in_idx = 1  # Need y[-1] at in_idx - 1, y[0] at in_idx, y[1] at in_idx + 1, y[2] at in_idx + 2

        while in_idx + 2 < len(input_samples):
            y_m1 = input_samples[in_idx - 1]
            y_0 = input_samples[in_idx]
            y_1 = input_samples[in_idx + 1]
            y_2 = input_samples[in_idx + 2]

            interp = cls.catmull_rom_sample(y_m1, y_0, y_1, y_2, phase)
            output.append(interp)

            phase += drift_ratio
            while phase >= 1.0 and in_idx + 2 < len(input_samples):
                phase -= 1.0
                in_idx += 1

        return output

    @staticmethod
    def cosine_fade_out_weights(length: int = 128) -> List[float]:
        """Generates 128-sample equal-power cosine fade-out curve [1.0 -> 0.0]."""
        return [0.5 * (1.0 + math.cos(math.pi * i / length)) for i in range(length)]

    @staticmethod
    def cosine_fade_in_weights(length: int = 128) -> List[float]:
        """Generates 128-sample equal-power cosine fade-in curve [0.0 -> 1.0]."""
        return [0.5 * (1.0 - math.cos(math.pi * i / length)) for i in range(length)]

    @classmethod
    def calculate_rms_and_peak(cls, samples: List[float]) -> Tuple[float, float]:
        """Calculates RMS level and peak absolute amplitude for an audio sample block."""
        if not samples:
            return 0.0, 0.0
        peak = max(abs(s) for s in samples)
        sum_sq = sum(s * s for s in samples)
        rms = math.sqrt(sum_sq / len(samples))
        return rms, peak

    @classmethod
    def amplitude_to_dbfs(cls, amplitude: float, floor_db: float = -60.0) -> float:
        """Converts linear amplitude to decibels full scale (dBFS), clamped to floor_db."""
        if amplitude <= 0.001:  # Floor at 0.001 = -60 dBFS
            return floor_db
        db = 20.0 * math.log10(amplitude)
        return max(floor_db, min(0.0, db))

    @staticmethod
    def rfc3550_jitter_update(
        prev_jitter: float,
        time_arrival: float,
        time_last_arrival: float,
        samples_per_frame: int,
        sample_rate: int,
        delta_frames: int = 1,
    ) -> float:
        """Updates RFC 3550 EWMA inter-arrival jitter metric."""
        if sample_rate <= 0:
            return prev_jitter
        expected_gap = (samples_per_frame / sample_rate) * delta_frames
        actual_gap = time_arrival - time_last_arrival
        diff = abs(actual_gap - expected_gap)
        # RFC 3550: J(i) = J(i-1) + (|D(i-1, i)| - J(i-1)) / 16
        return prev_jitter + (diff - prev_jitter) / 16.0


class VbanPacketValidator:
    """Implements the strict 10-step packet validation pipeline."""

    def __init__(self, target_stream_name: str = "Stream1"):
        self.target_stream_name = target_stream_name.strip()
        self.last_nu_frame: Optional[int] = None
        self.total_packets_received = 0
        self.lost_packets = 0
        self.out_of_order_packets = 0

    def validate_and_parse(
        self,
        datagram: bytes,
    ) -> Tuple[bool, Optional[str], Optional[Dict]]:
        """Executes the 10-step VBAN packet validation pipeline.
        
        Returns:
            (is_valid, drop_reason_if_invalid, parsed_info_dict_if_valid)
        """
        # Step 1: Minimum Header Length (28 bytes)
        if len(datagram) < 28:
            return False, "DROP_LENGTH_UNDER_28", None

        # Step 2: Magic FourCC Check ('VBAN')
        magic = datagram[:4]
        if magic != b"VBAN":
            return False, "DROP_INVALID_MAGIC", None

        # Parse header fields
        format_sr = datagram[4]
        format_nbs = datagram[5]
        format_nbc = datagram[6]
        format_bit = datagram[7]
        stream_name_raw = datagram[8:24]
        nu_frame = struct.unpack("<I", datagram[24:28])[0]

        # Step 3: Subprotocol Verification (Audio = 0x00)
        subprotocol = format_sr & 0xE0
        if subprotocol != VBAN_PROTOCOL_AUDIO:
            return False, f"DROP_NON_AUDIO_SUBPROTOCOL_{hex(subprotocol)}", None

        # Step 4: Codec Check (PCM = 0x00)
        codec = format_bit & 0xF0
        if codec != VBAN_CODEC_PCM:
            return False, f"DROP_UNSUPPORTED_CODEC_{hex(codec)}", None

        # Step 5: Reserved Bit 3 Check (must be 0)
        if (format_bit & 0x08) != 0:
            return False, "DROP_RESERVED_BIT3_SET", None

        # Step 6: Sample Rate Index Check (0..21 valid)
        sr_index = format_sr & 0x1F
        if sr_index >= len(VBAN_SR_TABLE):
            return False, f"DROP_INVALID_SR_INDEX_{sr_index}", None
        sample_rate = VBAN_SR_TABLE[sr_index]

        # Step 7: Data Type Supported Check
        data_type = format_bit & 0x07
        if data_type not in VBAN_SAMPLE_SIZES:
            return False, f"DROP_UNSUPPORTED_DATATYPE_{data_type}", None
        sample_size = VBAN_SAMPLE_SIZES[data_type]

        # Step 8: Stream Name Check (ASCII null-padded)
        stream_name = stream_name_raw.decode("ascii", errors="replace").rstrip("\x00 \t\r\n")
        if self.target_stream_name and stream_name.lower() != self.target_stream_name.lower():
            return False, f"DROP_UNSUBSCRIBED_STREAM_{stream_name}", None

        # Step 9: Exact Payload Size Validation
        samples_per_frame = format_nbs + 1
        channel_count = format_nbc + 1
        expected_samples = samples_per_frame * channel_count
        expected_payload_bytes = expected_samples * sample_size
        expected_total_bytes = 28 + expected_payload_bytes

        if len(datagram) != expected_total_bytes:
            if len(datagram) < expected_total_bytes:
                return False, f"DROP_TRUNCATED_PAYLOAD_{len(datagram)}_VS_{expected_total_bytes}", None
            else:
                return False, f"DROP_TRAILING_GARBAGE_{len(datagram)}_VS_{expected_total_bytes}", None

        # Step 10: Frame Sequence & Loss Tracking
        self.total_packets_received += 1
        if self.last_nu_frame is None:
            self.last_nu_frame = nu_frame
        else:
            delta = (nu_frame - self.last_nu_frame) & 0xFFFFFFFF
            if delta == 1:
                # In order
                pass
            elif 1 < delta < 0x80000000:
                # Loss detected
                lost = delta - 1
                self.lost_packets += lost
            elif delta == 0:
                # Duplicate packet
                self.out_of_order_packets += 1
            else:
                # Late / out of order
                self.out_of_order_packets += 1
            self.last_nu_frame = nu_frame

        # Extract and dequantize audio samples
        raw_payload = datagram[28:]
        samples = AudioDspOracle.dequantize_payload(raw_payload, data_type)

        parsed_info = {
            "sample_rate": sample_rate,
            "samples_per_frame": samples_per_frame,
            "channels": channel_count,
            "data_type": data_type,
            "stream_name": stream_name,
            "nu_frame": nu_frame,
            "samples": samples,
            "total_packets_received": self.total_packets_received,
            "lost_packets": self.lost_packets,
        }
        return True, None, parsed_info
