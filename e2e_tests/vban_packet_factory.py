"""VBAN Packet Factory for E2E Test Suite.

Provides packet generation, binary serialization, and validation helpers
strictly conforming to VB-Audio Network (VBAN) Protocol Specification (Rev 13).
"""

import struct
from typing import List, Optional, Tuple, Union

# Official 21 Sample Rates Table + 768 kHz Extension (Index 21)
VBAN_SR_TABLE: Tuple[int, ...] = (
    # 6 kHz family (Indices 0..6)
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    # 8 kHz family (Indices 7..13)
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    # 11.025 kHz family (Indices 14..20)
    11025, 22050, 44100, 88200, 176400, 352800, 705600,
    # Extended 768 kHz rate (Index 21)
    768000,
)

# Subprotocol constants
VBAN_PROTOCOL_AUDIO = 0x00
VBAN_PROTOCOL_SERIAL = 0x20
VBAN_PROTOCOL_TXT = 0x40
VBAN_PROTOCOL_SERVICE = 0x60

# Codec constants
VBAN_CODEC_PCM = 0x00
VBAN_CODEC_VBCA = 0x10
VBAN_CODEC_VBCV = 0x20

# Data Type constants
VBAN_DATATYPE_BYTE8 = 0x00
VBAN_DATATYPE_INT16 = 0x01
VBAN_DATATYPE_INT24 = 0x02
VBAN_DATATYPE_INT32 = 0x03
VBAN_DATATYPE_FLOAT32 = 0x04
VBAN_DATATYPE_FLOAT64 = 0x05
VBAN_DATATYPE_12BITS = 0x06
VBAN_DATATYPE_10BITS = 0x07

VBAN_SAMPLE_SIZES = {
    VBAN_DATATYPE_BYTE8: 1,
    VBAN_DATATYPE_INT16: 2,
    VBAN_DATATYPE_INT24: 3,
    VBAN_DATATYPE_INT32: 4,
    VBAN_DATATYPE_FLOAT32: 4,
    VBAN_DATATYPE_FLOAT64: 8,
    VBAN_DATATYPE_12BITS: 2,
    VBAN_DATATYPE_10BITS: 2,
}

VBAN_HEADER_SIZE = 28
VBAN_MAGIC_BYTES = b"VBAN"
VBAN_MAGIC_LE_INT = 0x4E414256


class VbanPacketFactory:
    """Builder for binary VBAN datagrams."""

    @staticmethod
    def get_sample_rate(sr_index: int) -> Optional[int]:
        if 0 <= sr_index < len(VBAN_SR_TABLE):
            return VBAN_SR_TABLE[sr_index]
        return None

    @staticmethod
    def get_sample_rate_index(sample_rate: int) -> Optional[int]:
        try:
            return VBAN_SR_TABLE.index(sample_rate)
        except ValueError:
            return None

    @staticmethod
    def pack_header(
        magic: bytes = VBAN_MAGIC_BYTES,
        subprotocol: int = VBAN_PROTOCOL_AUDIO,
        sr_index: int = 3,  # 48000 Hz default
        nbs: int = 127,     # 128 samples per frame
        nbc: int = 1,       # 2 channels (stereo)
        codec: int = VBAN_CODEC_PCM,
        bit3_reserved: int = 0,
        data_type: int = VBAN_DATATYPE_FLOAT32,
        stream_name: str = "Stream1",
        nu_frame: int = 0,
    ) -> bytes:
        """Packs a 28-byte VBAN header."""
        format_sr = (subprotocol & 0xE0) | (sr_index & 0x1F)
        format_nbs = nbs & 0xFF
        format_nbc = nbc & 0xFF
        format_bit = (codec & 0xF0) | ((bit3_reserved & 0x01) << 3) | (data_type & 0x07)

        # Stream name: exactly 16 bytes, null-padded ASCII
        name_bytes = stream_name.encode("ascii", errors="replace")[:16]
        stream_name_padded = name_bytes.ljust(16, b"\x00")

        # Format: 4s (magic), B (format_SR), B (format_nbs), B (format_nbc), B (format_bit), 16s (streamName), I (nuFrame)
        header = struct.pack(
            "<4sBBBB16sI",
            magic[:4],
            format_sr,
            format_nbs,
            format_nbc,
            format_bit,
            stream_name_padded,
            nu_frame & 0xFFFFFFFF,
        )
        assert len(header) == 28, f"Header size must be 28 bytes, got {len(header)}"
        return header

    @staticmethod
    def pack_payload(
        samples: List[float],
        data_type: int = VBAN_DATATYPE_FLOAT32,
    ) -> bytes:
        """Packs normalized Float32 samples [-1.0f, +1.0f] into the specified VBAN wire format."""
        payload = bytearray()
        if data_type == VBAN_DATATYPE_INT16:
            for s in samples:
                val = int(round(s * 32768.0))
                val = max(-32768, min(32767, val))
                payload.extend(struct.pack("<h", val))
        elif data_type == VBAN_DATATYPE_INT24:
            for s in samples:
                val = int(round(s * 8388608.0))
                val = max(-8388608, min(8388607, val))
                u24 = val & 0xFFFFFF
                b0 = u24 & 0xFF
                b1 = (u24 >> 8) & 0xFF
                b2 = (u24 >> 16) & 0xFF
                payload.extend(bytes([b0, b1, b2]))
        elif data_type == VBAN_DATATYPE_INT32:
            for s in samples:
                val = int(round(s * 2147483648.0))
                val = max(-2147483648, min(2147483647, val))
                payload.extend(struct.pack("<i", val))
        elif data_type == VBAN_DATATYPE_FLOAT32:
            for s in samples:
                payload.extend(struct.pack("<f", float(s)))
        elif data_type == VBAN_DATATYPE_BYTE8:
            for s in samples:
                # 8-bit unsigned, bias 128
                val = int(round(s * 128.0 + 128.0))
                val = max(0, min(255, val))
                payload.append(val)
        else:
            raise ValueError(f"Unsupported data_type: {data_type}")
        return bytes(payload)

    @classmethod
    def create_packet(
        cls,
        sample_rate: int = 48000,
        samples_per_frame: int = 128,
        channels: int = 2,
        data_type: int = VBAN_DATATYPE_FLOAT32,
        stream_name: str = "Stream1",
        nu_frame: int = 0,
        samples_data: Optional[List[float]] = None,
        subprotocol: int = VBAN_PROTOCOL_AUDIO,
        codec: int = VBAN_CODEC_PCM,
        bit3_reserved: int = 0,
        magic: bytes = VBAN_MAGIC_BYTES,
    ) -> bytes:
        """Builds a complete, valid VBAN packet."""
        sr_index = cls.get_sample_rate_index(sample_rate)
        if sr_index is None:
            sr_index = 3  # fallback 48kHz
        nbs = samples_per_frame - 1
        nbc = channels - 1

        total_samples = samples_per_frame * channels
        if samples_data is None:
            # Generate silence / zero samples by default
            samples_data = [0.0] * total_samples
        elif len(samples_data) < total_samples:
            samples_data = list(samples_data) + [0.0] * (total_samples - len(samples_data))
        else:
            samples_data = samples_data[:total_samples]

        header = cls.pack_header(
            magic=magic,
            subprotocol=subprotocol,
            sr_index=sr_index,
            nbs=nbs,
            nbc=nbc,
            codec=codec,
            bit3_reserved=bit3_reserved,
            data_type=data_type,
            stream_name=stream_name,
            nu_frame=nu_frame,
        )
        payload = cls.pack_payload(samples_data, data_type=data_type)
        return header + payload
