#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <string_view>
#include <cstring>
#include <cctype>
#include <ostream>

namespace vban {

// ==============================================================================
// VBAN Protocol Constants
// ==============================================================================

/**
 * FourCC magic bytes 'V', 'B', 'A', 'N' in Little-Endian byte order (0x4E414256).
 */
inline constexpr uint32_t VBAN_MAGIC = 0x4E414256U;

/**
 * Fixed length of the VBAN binary header in bytes.
 */
inline constexpr size_t VBAN_HEADER_SIZE = 28;

/**
 * Maximum capacity of the streamName character buffer.
 */
inline constexpr size_t VBAN_STREAM_NAME_MAX_LEN = 16;

/**
 * Maximum number of official sample rates defined in VB-Audio VBAN specification.
 */
inline constexpr size_t VBAN_OFFICIAL_SR_COUNT = 21;

/**
 * Extended sample rate table size including the 768 kHz audiophile extension.
 */
inline constexpr size_t VBAN_EXTENDED_SR_COUNT = 22;

// ==============================================================================
// Bitmasks & Enums
// ==============================================================================

inline constexpr uint8_t VBAN_SUBPROTOCOL_MASK    = 0xE0;
inline constexpr uint8_t VBAN_SR_INDEX_MASK       = 0x1F;
inline constexpr uint8_t VBAN_CODEC_MASK          = 0xF0;
inline constexpr uint8_t VBAN_RESERVED_BIT3_MASK  = 0x08;
inline constexpr uint8_t VBAN_DATATYPE_MASK       = 0x07;

enum class VBanSubProtocol : uint8_t {
    Audio   = 0x00,
    Serial  = 0x20,
    Text    = 0x40,
    Service = 0x60
};

enum class VBanCodec : uint8_t {
    PCM  = 0x00,
    VBCA = 0x10,
    VBCV = 0x20
};

enum class VBanDataType : uint8_t {
    Byte8   = 0x00,
    Int16   = 0x01,
    Int24   = 0x02,
    Int32   = 0x03,
    Float32 = 0x04,
    Float64 = 0x05,
    Bits12  = 0x06,
    Bits10  = 0x07
};

inline std::ostream& operator<<(std::ostream& os, VBanSubProtocol sp) {
    return os << static_cast<int>(sp);
}

inline std::ostream& operator<<(std::ostream& os, VBanCodec c) {
    return os << static_cast<int>(c);
}

inline std::ostream& operator<<(std::ostream& os, VBanDataType dt) {
    return os << static_cast<int>(dt);
}

// ==============================================================================
// Packed VBAN Header Structure (28 Bytes)
// ==============================================================================

#pragma pack(push, 1)
struct VBanHeader {
    uint32_t magic;          // Bytes 0..3: ASCII 'V', 'B', 'A', 'N' (0x4E414256 LE)
    uint8_t  format_SR;      // Byte 4:     Bits 7..5 = Subprotocol, Bits 4..0 = SR index
    uint8_t  format_nbs;     // Byte 5:     Sample frames per packet - 1 (0..255 -> 1..256)
    uint8_t  format_nbc;     // Byte 6:     Channels per frame - 1 (0..255 -> 1..256)
    uint8_t  format_bit;     // Byte 7:     Bits 7..4 = Codec, Bit 3 = Reserved (0), Bits 2..0 = DataType
    char     streamName[16]; // Bytes 8..23: ASCII stream name (null-terminated or padded)
    uint32_t nuFrame;        // Bytes 24..27: Frame sequence counter (Little-Endian uint32)
};
#pragma pack(pop)

static_assert(sizeof(VBanHeader) == 28, "VBanHeader must be exactly 28 bytes packed");
static_assert(offsetof(VBanHeader, magic) == 0, "magic offset must be 0");
static_assert(offsetof(VBanHeader, format_SR) == 4, "format_SR offset must be 4");
static_assert(offsetof(VBanHeader, format_nbs) == 5, "format_nbs offset must be 5");
static_assert(offsetof(VBanHeader, format_nbc) == 6, "format_nbc offset must be 6");
static_assert(offsetof(VBanHeader, format_bit) == 7, "format_bit offset must be 7");
static_assert(offsetof(VBanHeader, streamName) == 8, "streamName offset must be 8");
static_assert(offsetof(VBanHeader, nuFrame) == 24, "nuFrame offset must be 24");

// ==============================================================================
// Sample Rate & Size Tables
// ==============================================================================

/**
 * Authoritative VBAN Sample Rate Table:
 * Indices 0..6:   6 kHz multiplier family (6k to 384k)
 * Indices 7..13:  8 kHz multiplier family (8k to 512k)
 * Indices 14..20: 11.025 kHz multiplier family (11.025k to 705.6k)
 * Index 21:       Audiophile 768 kHz extension
 */
inline constexpr std::array<uint32_t, VBAN_EXTENDED_SR_COUNT> VBAN_SR_TABLE = {
    // 6 kHz base family
    6000, 12000, 24000, 48000, 96000, 192000, 384000,
    // 8 kHz base family
    8000, 16000, 32000, 64000, 128000, 256000, 512000,
    // 11.025 kHz base family
    11025, 22050, 44100, 88200, 176400, 352800, 705600,
    // Audiophile 768 kHz extension
    768000
};

/**
 * Byte sizes indexed by VBanDataType (0x00 to 0x07).
 */
inline constexpr std::array<uint8_t, 8> VBAN_SAMPLE_SIZES = {
    1, // 0x00: Byte8
    2, // 0x01: Int16
    3, // 0x02: Int24
    4, // 0x03: Int32
    4, // 0x04: Float32
    8, // 0x05: Float64
    2, // 0x06: Bits12
    2  // 0x07: Bits10
};

// ==============================================================================
// Constexpr Helper Functions
// ==============================================================================

[[nodiscard]] inline constexpr VBanSubProtocol getSubProtocol(uint8_t format_SR) noexcept {
    return static_cast<VBanSubProtocol>(format_SR & VBAN_SUBPROTOCOL_MASK);
}

[[nodiscard]] inline constexpr uint8_t getSampleRateIndex(uint8_t format_SR) noexcept {
    return format_SR & VBAN_SR_INDEX_MASK;
}

[[nodiscard]] inline constexpr std::optional<uint32_t> getSampleRate(uint8_t format_SR) noexcept {
    uint8_t idx = getSampleRateIndex(format_SR);
    if (idx < VBAN_EXTENDED_SR_COUNT) {
        return VBAN_SR_TABLE[idx];
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr VBanCodec getCodec(uint8_t format_bit) noexcept {
    return static_cast<VBanCodec>(format_bit & VBAN_CODEC_MASK);
}

[[nodiscard]] inline constexpr bool isReservedBitSet(uint8_t format_bit) noexcept {
    return (format_bit & VBAN_RESERVED_BIT3_MASK) != 0;
}

[[nodiscard]] inline constexpr VBanDataType getDataType(uint8_t format_bit) noexcept {
    return static_cast<VBanDataType>(format_bit & VBAN_DATATYPE_MASK);
}

[[nodiscard]] inline constexpr uint8_t getSampleSize(VBanDataType type) noexcept {
    return VBAN_SAMPLE_SIZES[static_cast<uint8_t>(type) & 0x07];
}

[[nodiscard]] inline constexpr uint8_t getSampleSize(uint8_t format_bit) noexcept {
    return VBAN_SAMPLE_SIZES[format_bit & VBAN_DATATYPE_MASK];
}

[[nodiscard]] inline constexpr size_t calculatePayloadBytes(uint8_t format_nbs, uint8_t format_nbc, uint8_t sampleSize) noexcept {
    return static_cast<size_t>(format_nbs + 1) * 
           static_cast<size_t>(format_nbc + 1) * 
           static_cast<size_t>(sampleSize);
}

[[nodiscard]] inline constexpr size_t calculateTotalPacketBytes(uint8_t format_nbs, uint8_t format_nbc, uint8_t sampleSize) noexcept {
    return VBAN_HEADER_SIZE + calculatePayloadBytes(format_nbs, format_nbc, sampleSize);
}

/**
 * Extracts and cleans a stream name from the fixed 16-byte header field.
 * Safely caps at 16 bytes, trims null terminators and trailing whitespace.
 */
[[nodiscard]] inline std::string_view extractStreamName(const char streamName[16]) noexcept {
    size_t len = 0;
    while (len < VBAN_STREAM_NAME_MAX_LEN && streamName[len] != '\0') {
        ++len;
    }
    // Trim trailing ASCII whitespace
    while (len > 0 && (streamName[len - 1] == ' '  || 
                       streamName[len - 1] == '\t' || 
                       streamName[len - 1] == '\r' || 
                       streamName[len - 1] == '\n')) {
        --len;
    }
    return std::string_view(streamName, len);
}

} // namespace vban
