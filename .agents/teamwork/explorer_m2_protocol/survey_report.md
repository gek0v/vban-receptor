# Milestone 2: VBAN Protocol Header & Parser Implementation Architecture

## Executive Summary

This report establishes the complete, production-ready C++20 architecture and implementation templates for **Milestone 2: VBAN Protocol Header & Parser Core**. 
The design strictly follows the official VB-Audio Network (VBAN) specification (Revision 13), `PROJECT.md`, `sub_orch_m2/SCOPE.md`, and the survey findings in `spec_miner_vban/survey_report.md`.

The architecture encompasses four tightly coupled deliverables:
1. `app/src/main/cpp/include/vban_protocol.hpp`: Exact 28-byte packed `VBanHeader` struct, protocol constants, sample rate lookup table (21 official rates + 768 kHz audiophile extension), sample size mapping, and constexpr calculation helpers.
2. `app/src/main/cpp/include/vban_parser.hpp`: Header declaration of the `VbanParser` engine, `ParsedPacket` view, `ParseResult` status codes, and telemetry structures.
3. `app/src/main/cpp/src/vban_parser.cpp`: Implementation of the strict 10-step datagram validation pipeline, case-insensitive stream name matching, 32-bit monotonic sequence loss tracking with unsigned wrap handling, and RFC 3550 EWMA inter-arrival jitter estimation.
4. `native_tests/test_vban_protocol.cpp`: Exhaustive unit test suite built against the project's native test framework (`native_tests/test_framework.hpp`), exercising 100% of header constraints, lookup tables, edge cases, malformed packets, and sequence/jitter algorithms.

---

## 1. Protocol Specification & Memory Layout

### 1.1 VBanHeader Binary Layout (28 Bytes Packed)

The VBAN packet header is fixed at exactly 28 bytes preceding the audio payload. No padding bytes may be inserted by the compiler. `#pragma pack(push, 1)` / `#pragma pack(pop)` is enforced with static assertions on size and every field offset.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       'V'     |       'B'     |       'A'     |       'N'     | magic (0x4E414256 LE)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| SubP | SR-Idx |   format_nbs  |   format_nbc  | Codec |R|DataType| format bytes
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                       streamName[16]                          + streamName (ASCII, null-padded)
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            nuFrame                            | nuFrame (uint32_t LE)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Audio Payload                         |
|      (Interleaved: format_nbs+1 x format_nbc+1 x sampleSize)  |
|                             ...                               |
```

### 1.2 Byte-by-Byte Field Definition

| Field Name | Type | Offset | Size | Bitmask / Subfields | Description |
|:---|:---:|:---:|:---:|:---|:---|
| `magic` | `uint32_t` | 0 | 4 | Little-Endian `0x4E414256` | FourCC ASCII sequence `'V'`, `'B'`, `'A'`, `'N'`. |
| `format_SR` | `uint8_t` | 4 | 1 | Bits 7..5: Subprotocol (`0xE0`)<br>Bits 4..0: SR Index (`0x1F`) | `0x00` = VBAN Audio. Indices `0..20` = 21 official rates; index `21` = 768 kHz. |
| `format_nbs` | `uint8_t` | 5 | 1 | Full byte `0..255` | Number of sample frames minus 1 (`0` -> 1 sample frame, `255` -> 256 sample frames). |
| `format_nbc` | `uint8_t` | 6 | 1 | Full byte `0..255` | Number of channels minus 1 (`0` -> 1 channel, `1` -> 2 channels / stereo, `7` -> 8 channels). |
| `format_bit` | `uint8_t` | 7 | 1 | Bits 7..4: Codec (`0xF0`)<br>Bit 3: Reserved (`0x08`)<br>Bits 2..0: Data Type (`0x07`) | Codec: `0x00` = PCM.<br>Bit 3: Must be `0`.<br>Data Type: `0x01`=Int16, `0x02`=Int24, `0x03`=Int32, `0x04`=Float32. |
| `streamName` | `char[16]` | 8 | 16 | Fixed 16-byte ASCII array | Stream identifier. Null-terminated or null-padded. |
| `nuFrame` | `uint32_t` | 24 | 4 | Little-Endian `uint32_t` | Monotonic 32-bit frame counter incremented by 1 per packet. |

---

## 2. Sample Rate Table & Payload Mathematics

### 2.1 Complete Sample Rate Lookup Table (`VBAN_SR_TABLE`)

The official VBAN specification provides 21 standard sample rates divided into three base multiplier families (6 kHz, 8 kHz, and 11.025 kHz). Index 21 is the established extension for audiophile 768 kHz hardware DACs.

| Index | Frequency (Hz) | Family | Multiplier | Class / Usage |
|:---:|:---:|:---:|:---:|:---|
| 0 | **6,000** | 6 kHz | $6000 \times 1$ | Ultra-low bandwidth telephony |
| 1 | **12,000** | 6 kHz | $6000 \times 2$ | Low bandwidth speech |
| 2 | **24,000** | 6 kHz | $6000 \times 4$ | Standard communications |
| 3 | **48,000** | 6 kHz | $6000 \times 8$ | Standard broadcast / video audio |
| 4 | **96,000** | 6 kHz | $6000 \times 16$ | High-Resolution pro audio |
| 5 | **192,000** | 6 kHz | $6000 \times 32$ | Ultra-High-Resolution studio master |
| 6 | **384,000** | 6 kHz | $6000 \times 64$ | Audiophile DXD / Studio master |
| 7 | **8,000** | 8 kHz | $8000 \times 1$ | Narrowband PSTN telephony (G.711) |
| 8 | **16,000** | 8 kHz | $8000 \times 2$ | Wideband speech (G.722) |
| 9 | **32,000** | 8 kHz | $8000 \times 4$ | Broadcast FM / Digital radio (NICAM) |
| 10 | **64,000** | 8 kHz | $8000 \times 8$ | Professional telecom |
| 11 | **128,000** | 8 kHz | $8000 \times 16$ | High-frequency acoustic analysis |
| 12 | **256,000** | 8 kHz | $8000 \times 32$ | Ultra-wideband ultrasonic sampling |
| 13 | **512,000** | 8 kHz | $8000 \times 64$ | Scientific measurement / Ultrasonic |
| 14 | **11,025** | 11.025 kHz | $11025 \times 1$ | Legacy multimedia |
| 15 | **22,050** | 11.025 kHz | $11025 \times 2$ | FM radio quality |
| 16 | **44,100** | 11.025 kHz | $11025 \times 4$ | Red Book Audio CD standard |
| 17 | **88,200** | 11.025 kHz | $11025 \times 8$ | High-Resolution music production (2x CD) |
| 18 | **176,400** | 11.025 kHz | $11025 \times 16$ | High-Resolution music production (4x CD) |
| 19 | **352,800** | 11.025 kHz | $11025 \times 32$ | DXD (Digital eXtreme Definition / 8x CD) |
| 20 | **705,600** | 11.025 kHz | $11025 \times 64$ | Ultra-Audiophile 16x CD / Master clock |
| 21 | **768,000** | 6 kHz / 48 kHz | $48000 \times 16$ | **Audiophile Extended Rate (USB DAC 768k)** |

### 2.2 Data Types & Sample Sizes

| Data Type Constant | Value | Size (Bytes) | Audio Format Description | Support Status in Audio Core |
|:---|:---:|:---:|:---|:---:|
| `VBAN_DATATYPE_BYTE8` | `0x00` | 1 | 8-bit unsigned integer (`uint8_t`) | Unsupported in audiophile pipeline |
| `VBAN_DATATYPE_INT16` | `0x01` | 2 | 16-bit signed integer (LE, 2's complement) | **Fully Supported** |
| `VBAN_DATATYPE_INT24` | `0x02` | 3 | 24-bit packed signed integer (LE, 3 bytes) | **Fully Supported** |
| `VBAN_DATATYPE_INT32` | `0x03` | 4 | 32-bit signed integer (LE, 2's complement) | **Fully Supported** |
| `VBAN_DATATYPE_FLOAT32` | `0x04` | 4 | 32-bit IEEE 754 single-precision float | **Fully Supported** |
| `VBAN_DATATYPE_FLOAT64` | `0x05` | 8 | 64-bit IEEE 754 double-precision float | Unsupported |
| `VBAN_DATATYPE_12BITS` | `0x06` | 2 | 12-bit signed in 16-bit container | Unsupported |
| `VBAN_DATATYPE_10BITS` | `0x07` | 2 | 10-bit signed in 16-bit container | Unsupported |

### 2.3 Payload Calculation Formulas

Let $N_s = \text{format\_nbs} + 1 \in [1, 256]$ be the sample frames per packet.  
Let $N_c = \text{format\_nbc} + 1 \in [1, 256]$ be the channel count.  
Let $B_s = \text{sampleSize} \in \{2, 3, 4\}$ be the sample size in bytes.

$$\text{Payload Bytes} = N_s \times N_c \times B_s$$
$$\text{Total Expected Datagram Size} = 28 + \text{Payload Bytes}$$

---

## 3. The Strict 10-Step Packet Validation Pipeline

Every received datagram is validated sequentially. A failure at any step causes immediate rejection without allocating memory or advancing audio state:

```
[Incoming Datagram (size bytes)]
              │
              ▼
    [Step 1: size >= 28?] ──────────► NO ──► ParseResult::PacketTooSmall
              │ YES
              ▼
    [Step 2: magic == 'VBAN'?] ─────► NO ──► ParseResult::InvalidMagic
              │ YES
              ▼
    [Step 3: subprotocol == 0x00?] ─► NO ──► ParseResult::UnsupportedSubprotocol
              │ YES
              ▼
    [Step 4: codec == 0x00 (PCM)?] ─► NO ──► ParseResult::UnsupportedCodec
              │ YES
              ▼
    [Step 5: format_bit & 0x08 == 0?]►NO ──► ParseResult::ReservedBitSet
              │ YES
              ▼
    [Step 6: srIndex < 22?] ────────► NO ──► ParseResult::InvalidSampleRate
              │ YES
              ▼
    [Step 7: dataType in {1,2,3,4}?]► NO ──► ParseResult::UnsupportedDataType
              │ YES
              ▼
    [Step 8: streamName matches?] ──► NO ──► ParseResult::StreamNameMismatch
              │ YES
              ▼
    [Step 9: size == 28 + payload?] ─► NO ──► ParseResult::PayloadSizeMismatch
              │ YES
              ▼
    [Step 10: nuFrame Loss Tracking & RFC 3550 Jitter Estimation]
              │
              ▼
    [ParseResult::Success -> Out to Dequantizer / SPSC Ring Buffer]
```

### 3.1 Step 10: Sequence Tracking & RFC 3550 Jitter Estimation

#### Sequence Loss Tracking (`nuFrame`)
- Let $C_{\text{cur}} = \text{header->nuFrame}$ and $C_{\text{prev}} = \text{lastFrame}$.
- Sequence difference is evaluated using 32-bit unsigned modular subtraction:
  $$\Delta = C_{\text{cur}} - C_{\text{prev}}$$
- Evaluation:
  - If $\Delta == 1$: Normal, sequential packet.
  - If $1 < \Delta < 0x80000000\text{U}$: Packet loss detected. The number of lost packets is $\Delta - 1$.
  - If $\Delta == 0$ or $\Delta \ge 0x80000000\text{U}$: Duplicate or out-of-order packet. Out-of-order counter is incremented, and packet is discarded or flagged to prevent audio corruption.
  - When $C_{\text{cur}}$ rolls over from $0\text{xFFFFFFFF}$ to $0\text{x00000000}$, $\Delta = 0\text{x00000000} - 0\text{xFFFFFFFF} = 1$, seamlessly identifying continuous in-order transmission without false packet loss alerts.

#### RFC 3550 Jitter Estimation
In RFC 3550 §6.4.1, the inter-arrival jitter $J$ measures the mean statistical deviation of packet arrival spacing compared to sender transmission spacing.
- Each VBAN packet carries $N_s$ samples per channel at sampling rate $f_s$ Hz. The nominal time duration of one packet is:
  $$T_{\text{frame}} = \frac{N_s}{f_s}\text{ seconds}$$
- When sequence advances by $\Delta$ packets, the nominal sender transmission interval is:
  $$\Delta S = \Delta \times \left(\frac{N_s}{f_s}\right)\text{ seconds}$$
- If $t_{\text{cur}}$ and $t_{\text{prev}}$ are packet reception timestamps from a monotonic clock (in nanoseconds), the measured arrival interval is:
  $$\Delta R = (t_{\text{cur}} - t_{\text{prev}}) \times 10^{-9}\text{ seconds}$$
- The transmission difference $D$ between arrival interval and transmission interval is:
  $$D = |\Delta R - \Delta S|$$
- The RFC 3550 exponential moving average filter update is:
  $$J_{\text{cur}} = J_{\text{prev}} + \frac{D - J_{\text{prev}}}{16.0}$$
- Output telemetry: $J_{\text{ms}} = J_{\text{cur}} \times 1000.0\text{ ms}$.

---

## 4. Production-Ready C++20 Code Templates

### 4.1 `app/src/main/cpp/include/vban_protocol.hpp`

```cpp
#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <optional>
#include <string_view>
#include <cstring>
#include <cctype>

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

inline constexpr uint8_t VBAN_SUBPROTOCOL_MASK = 0xE0;
inline constexpr uint8_t VBAN_SR_INDEX_MASK    = 0x1F;
inline constexpr uint8_t VBAN_CODEC_MASK       = 0xF0;
inline constexpr uint8_t VBAN_RESERVED_BIT3_MASK = 0x08;
inline constexpr uint8_t VBAN_DATATYPE_MASK    = 0x07;

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
```

---

### 4.2 `app/src/main/cpp/include/vban_parser.hpp`

```cpp
#pragma once

#include "vban_protocol.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <chrono>

namespace vban {

/**
 * Validation return codes covering the 10-step packet pipeline.
 */
enum class ParseResult {
    Success = 0,
    PacketTooSmall,           // Size < 28 bytes
    InvalidMagic,             // Magic != 0x4E414256 ('VBAN')
    UnsupportedSubprotocol,   // Subprotocol != Audio (0x00)
    UnsupportedCodec,         // Codec != PCM (0x00)
    ReservedBitSet,           // Bit 3 of format_bit is non-zero
    InvalidSampleRate,        // Sample rate index >= 22
    UnsupportedDataType,      // Data type is not Int16, Int24, Int32, or Float32
    StreamNameMismatch,       // Stream name does not match target filter
    PayloadSizeMismatch       // Received size != 28 + (nbs+1)*(nbc+1)*sampleSize
};

/**
 * Decoded and verified packet metadata referencing payload bytes directly.
 */
struct ParsedPacket {
    const VBanHeader* header{nullptr};
    const uint8_t*    payload{nullptr};
    size_t            payloadBytes{0};
    uint32_t          sampleRate{0};
    uint16_t          channels{0};         // nbc + 1 (1..256)
    uint16_t          samplesPerFrame{0};  // nbs + 1 (1..256)
    uint8_t           sampleSize{0};       // Bytes per sample
    VBanDataType      dataType{VBanDataType::Int16};
    std::string_view  streamName{};
    uint32_t          frameNumber{0};      // nuFrame
};

/**
 * Thread-safe sequence and network telemetry snapshot.
 */
struct ParserTelemetry {
    uint64_t totalPacketsReceived{0};
    uint64_t validPackets{0};
    uint64_t lostPackets{0};
    uint64_t outOfOrderPackets{0};
    double   jitterMs{0.0};
    uint32_t lastSequenceNumber{0};
};

/**
 * VbanParser
 * 
 * Implements the strict 10-step VBAN packet validation pipeline,
 * frame sequence tracking, unsigned 32-bit wrap handling, and RFC 3550
 * inter-arrival jitter estimation.
 * 
 * Designed for zero heap allocations during real-time streaming.
 */
class VbanParser {
public:
    explicit VbanParser(std::string_view targetStreamName = "");
    ~VbanParser() = default;

    // Non-copyable, movable
    VbanParser(const VbanParser&) = delete;
    VbanParser& operator=(const VbanParser&) = delete;
    VbanParser(VbanParser&&) noexcept = default;
    VbanParser& operator=(VbanParser&&) noexcept = default;

    /**
     * Set target stream name filter. Empty string disables filtering (accept all).
     */
    void setTargetStreamName(std::string_view targetStreamName);

    /**
     * Get configured target stream name.
     */
    [[nodiscard]] std::string_view getTargetStreamName() const noexcept;

    /**
     * Reset sequence tracking, loss counters, and jitter state.
     */
    void reset() noexcept;

    /**
     * Executes the strict 10-step validation pipeline against an incoming datagram.
     * 
     * @param buffer Raw datagram bytes.
     * @param size Total datagram byte count.
     * @param arrivalTimeNs Monotonic reception timestamp in nanoseconds.
     * @param outPacket Destination struct populated upon ParseResult::Success.
     * @return ParseResult status code.
     */
    [[nodiscard]] ParseResult parse(
        const uint8_t* buffer,
        size_t size,
        uint64_t arrivalTimeNs,
        ParsedPacket& outPacket) noexcept;

    /**
     * Convenience overload measuring arrival time via std::chrono::steady_clock.
     */
    [[nodiscard]] ParseResult parse(
        const uint8_t* buffer,
        size_t size,
        ParsedPacket& outPacket) noexcept;

    // Telemetry Accessors
    [[nodiscard]] uint64_t getTotalPackets() const noexcept { return totalPackets_; }
    [[nodiscard]] uint64_t getValidPackets() const noexcept { return validPackets_; }
    [[nodiscard]] uint64_t getLostPackets() const noexcept { return lostPackets_; }
    [[nodiscard]] uint64_t getOutOfOrderPackets() const noexcept { return outOfOrderPackets_; }
    [[nodiscard]] double getJitterMs() const noexcept { return jitterSeconds_ * 1000.0; }
    [[nodiscard]] double getJitterSeconds() const noexcept { return jitterSeconds_; }
    [[nodiscard]] ParserTelemetry getTelemetry() const noexcept;

    /**
     * Helper stringifier for status codes.
     */
    [[nodiscard]] static const char* resultToString(ParseResult result) noexcept;

private:
    [[nodiscard]] bool matchStreamName(std::string_view candidate) const noexcept;
    void updateSequenceAndJitter(uint32_t nuFrame, uint32_t sampleRate, uint16_t samplesPerFrame, uint64_t arrivalTimeNs) noexcept;

    std::string targetStreamName_;

    // Telemetry & Sequence Tracking
    uint64_t totalPackets_{0};
    uint64_t validPackets_{0};
    uint64_t lostPackets_{0};
    uint64_t outOfOrderPackets_{0};

    bool     hasReceivedFirst_{false};
    uint32_t lastNuFrame_{0};
    uint64_t lastArrivalNs_{0};
    double   jitterSeconds_{0.0};
};

} // namespace vban
```

---

### 4.3 `app/src/main/cpp/src/vban_parser.cpp`

```cpp
#include "vban_parser.hpp"
#include <cmath>
#include <algorithm>

namespace vban {

VbanParser::VbanParser(std::string_view targetStreamName)
    : targetStreamName_(targetStreamName) {}

void VbanParser::setTargetStreamName(std::string_view targetStreamName) {
    targetStreamName_ = std::string(targetStreamName);
}

std::string_view VbanParser::getTargetStreamName() const noexcept {
    return targetStreamName_;
}

void VbanParser::reset() noexcept {
    totalPackets_ = 0;
    validPackets_ = 0;
    lostPackets_ = 0;
    outOfOrderPackets_ = 0;
    hasReceivedFirst_ = false;
    lastNuFrame_ = 0;
    lastArrivalNs_ = 0;
    jitterSeconds_ = 0.0;
}

ParserTelemetry VbanParser::getTelemetry() const noexcept {
    return ParserTelemetry{
        .totalPacketsReceived = totalPackets_,
        .validPackets = validPackets_,
        .lostPackets = lostPackets_,
        .outOfOrderPackets = outOfOrderPackets_,
        .jitterMs = jitterSeconds_ * 1000.0,
        .lastSequenceNumber = lastNuFrame_
    };
}

const char* VbanParser::resultToString(ParseResult result) noexcept {
    switch (result) {
        case ParseResult::Success:                return "Success";
        case ParseResult::PacketTooSmall:          return "PacketTooSmall";
        case ParseResult::InvalidMagic:            return "InvalidMagic";
        case ParseResult::UnsupportedSubprotocol:  return "UnsupportedSubprotocol";
        case ParseResult::UnsupportedCodec:        return "UnsupportedCodec";
        case ParseResult::ReservedBitSet:          return "ReservedBitSet";
        case ParseResult::InvalidSampleRate:       return "InvalidSampleRate";
        case ParseResult::UnsupportedDataType:     return "UnsupportedDataType";
        case ParseResult::StreamNameMismatch:      return "StreamNameMismatch";
        case ParseResult::PayloadSizeMismatch:     return "PayloadSizeMismatch";
    }
    return "UnknownError";
}

bool VbanParser::matchStreamName(std::string_view candidate) const noexcept {
    if (targetStreamName_.empty()) {
        return true; // Wildcard / promiscuous mode
    }
    if (candidate.size() != targetStreamName_.size()) {
        return false;
    }
    // Case-insensitive ASCII comparison
    return std::equal(candidate.begin(), candidate.end(),
                      targetStreamName_.begin(), targetStreamName_.end(),
                      [](char a, char b) noexcept {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

void VbanParser::updateSequenceAndJitter(
    uint32_t nuFrame,
    uint32_t sampleRate,
    uint16_t samplesPerFrame,
    uint64_t arrivalTimeNs) noexcept {

    if (!hasReceivedFirst_) {
        hasReceivedFirst_ = true;
        lastNuFrame_ = nuFrame;
        lastArrivalNs_ = arrivalTimeNs;
        jitterSeconds_ = 0.0;
        return;
    }

    // 32-bit unsigned modular sequence difference
    const uint32_t delta = nuFrame - lastNuFrame_;

    if (delta == 1) {
        // Sequential packet: 0 loss
    } else if (delta > 1 && delta < 0x80000000U) {
        // Forward gap: packet loss detected
        lostPackets_ += (delta - 1);
    } else {
        // Out-of-order or duplicate packet (delta == 0 or delta >= 0x80000000U)
        outOfOrderPackets_++;
        // Do not advance lastNuFrame_ or update jitter for out-of-order packets
        return;
    }

    // RFC 3550 Inter-Arrival Jitter Estimation
    // D(i, j) = | (R_j - R_i) - (S_j - S_i) |
    // J(i) = J(i-1) + (|D| - J(i-1)) / 16.0
    if (sampleRate > 0 && arrivalTimeNs >= lastArrivalNs_) {
        const double elapsedArrivalSec = static_cast<double>(arrivalTimeNs - lastArrivalNs_) * 1e-9;
        const double nominalPacketSec  = static_cast<double>(samplesPerFrame) / static_cast<double>(sampleRate);
        const double nominalIntervalSec = static_cast<double>(delta) * nominalPacketSec;

        const double d = std::abs(elapsedArrivalSec - nominalIntervalSec);
        jitterSeconds_ += (d - jitterSeconds_) / 16.0;
    }

    lastNuFrame_ = nuFrame;
    lastArrivalNs_ = arrivalTimeNs;
}

ParseResult VbanParser::parse(
    const uint8_t* buffer,
    size_t size,
    uint64_t arrivalTimeNs,
    ParsedPacket& outPacket) noexcept {

    totalPackets_++;

    // Step 1: Minimum packet size (at least 28-byte header)
    if (buffer == nullptr || size < VBAN_HEADER_SIZE) {
        return ParseResult::PacketTooSmall;
    }

    const auto* header = reinterpret_cast<const VBanHeader*>(buffer);

    // Step 2: Magic FourCC check ('V','B','A','N' == 0x4E414256 LE)
    if (header->magic != VBAN_MAGIC) {
        return ParseResult::InvalidMagic;
    }

    // Step 3: Audio subprotocol check (0x00)
    if (getSubProtocol(header->format_SR) != VBanSubProtocol::Audio) {
        return ParseResult::UnsupportedSubprotocol;
    }

    // Step 4: Codec check (0x00 PCM)
    if (getCodec(header->format_bit) != VBanCodec::PCM) {
        return ParseResult::UnsupportedCodec;
    }

    // Step 5: Reserved Bit 3 check (must be 0)
    if (isReservedBitSet(header->format_bit)) {
        return ParseResult::ReservedBitSet;
    }

    // Step 6: Sample rate lookup (indices 0..21)
    const auto srOpt = getSampleRate(header->format_SR);
    if (!srOpt.has_value()) {
        return ParseResult::InvalidSampleRate;
    }
    const uint32_t sampleRate = *srOpt;

    // Step 7: Data type support check (Int16, Int24, Int32, Float32)
    const VBanDataType dataType = getDataType(header->format_bit);
    if (dataType != VBanDataType::Int16 &&
        dataType != VBanDataType::Int24 &&
        dataType != VBanDataType::Int32 &&
        dataType != VBanDataType::Float32) {
        return ParseResult::UnsupportedDataType;
    }

    // Step 8: Stream name matching
    const std::string_view streamName = extractStreamName(header->streamName);
    if (!matchStreamName(streamName)) {
        return ParseResult::StreamNameMismatch;
    }

    // Step 9: Exact payload size validation
    const uint8_t sampleSize = getSampleSize(dataType);
    const size_t expectedPayload = calculatePayloadBytes(header->format_nbs, header->format_nbc, sampleSize);
    const size_t expectedTotalBytes = VBAN_HEADER_SIZE + expectedPayload;

    if (size != expectedTotalBytes) {
        return ParseResult::PayloadSizeMismatch;
    }

    // Populate output metadata
    const uint16_t samplesPerFrame = static_cast<uint16_t>(header->format_nbs) + 1;
    const uint16_t channels        = static_cast<uint16_t>(header->format_nbc) + 1;

    outPacket.header          = header;
    outPacket.payload         = buffer + VBAN_HEADER_SIZE;
    outPacket.payloadBytes    = expectedPayload;
    outPacket.sampleRate      = sampleRate;
    outPacket.channels        = channels;
    outPacket.samplesPerFrame = samplesPerFrame;
    outPacket.sampleSize      = sampleSize;
    outPacket.dataType        = dataType;
    outPacket.streamName      = streamName;
    outPacket.frameNumber     = header->nuFrame;

    // Step 10: Monotonic frame loss tracking and RFC 3550 jitter calculation
    updateSequenceAndJitter(header->nuFrame, sampleRate, samplesPerFrame, arrivalTimeNs);
    validPackets_++;

    return ParseResult::Success;
}

ParseResult VbanParser::parse(
    const uint8_t* buffer,
    size_t size,
    ParsedPacket& outPacket) noexcept {

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nowNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return parse(buffer, size, nowNs, outPacket);
}

} // namespace vban
```

---

### 4.4 `native_tests/test_vban_protocol.cpp`

```cpp
#include "test_framework.hpp"
#include "vban_protocol.hpp"
#include "vban_parser.hpp"

#include <vector>
#include <cstring>
#include <cmath>

namespace {

/**
 * Helper generating a valid dummy VBAN packet for unit testing.
 */
std::vector<uint8_t> createVbanPacket(
    uint32_t srIndex = 3,       // Default 48 kHz
    uint8_t  nbs = 255,         // Default 256 samples
    uint8_t  nbc = 1,           // Default 2 channels (stereo)
    vban::VBanDataType dt = vban::VBanDataType::Int16,
    const char* streamName = "Stream1",
    uint32_t nuFrame = 0) {

    uint8_t sampleSize = vban::getSampleSize(dt);
    size_t payloadBytes = vban::calculatePayloadBytes(nbs, nbc, sampleSize);
    std::vector<uint8_t> packet(vban::VBAN_HEADER_SIZE + payloadBytes, 0);

    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    hdr->magic = vban::VBAN_MAGIC;
    hdr->format_SR = (static_cast<uint8_t>(vban::VBanSubProtocol::Audio)) | (srIndex & 0x1F);
    hdr->format_nbs = nbs;
    hdr->format_nbc = nbc;
    hdr->format_bit = (static_cast<uint8_t>(vban::VBanCodec::PCM)) | (static_cast<uint8_t>(dt) & 0x07);
    
    std::memset(hdr->streamName, 0, 16);
    if (streamName) {
        std::strncpy(hdr->streamName, streamName, 16);
    }
    hdr->nuFrame = nuFrame;

    return packet;
}

} // anonymous namespace

// ==============================================================================
// 1. Structure Layout & Size Tests
// ==============================================================================

TEST_CASE("vban_header_size_and_field_offsets") {
    ASSERT_EQ(sizeof(vban::VBanHeader), 28U);
    ASSERT_EQ(offsetof(vban::VBanHeader, magic), 0U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_SR), 4U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_nbs), 5U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_nbc), 6U);
    ASSERT_EQ(offsetof(vban::VBanHeader, format_bit), 7U);
    ASSERT_EQ(offsetof(vban::VBanHeader, streamName), 8U);
    ASSERT_EQ(offsetof(vban::VBanHeader, nuFrame), 24U);
}

TEST_CASE("vban_magic_constant") {
    ASSERT_EQ(vban::VBAN_MAGIC, 0x4E414256U);
    
    // Check ASCII byte ordering
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&vban::VBAN_MAGIC);
    ASSERT_EQ(bytes[0], 'V');
    ASSERT_EQ(bytes[1], 'B');
    ASSERT_EQ(bytes[2], 'A');
    ASSERT_EQ(bytes[3], 'N');
}

// ==============================================================================
// 2. Sample Rate Table & Extensions
// ==============================================================================

TEST_CASE("vban_sr_table_all_21_official_rates") {
    ASSERT_EQ(vban::VBAN_OFFICIAL_SR_COUNT, 21U);
    
    // 6 kHz family
    ASSERT_EQ(vban::VBAN_SR_TABLE[0], 6000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[1], 12000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[2], 24000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[3], 48000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[4], 96000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[5], 192000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[6], 384000U);

    // 8 kHz family
    ASSERT_EQ(vban::VBAN_SR_TABLE[7], 8000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[8], 16000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[9], 32000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[10], 64000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[11], 128000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[12], 256000U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[13], 512000U);

    // 11.025 kHz family
    ASSERT_EQ(vban::VBAN_SR_TABLE[14], 11025U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[15], 22050U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[16], 44100U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[17], 88200U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[18], 176400U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[19], 352800U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[20], 705600U);
}

TEST_CASE("vban_sr_table_768khz_extension") {
    ASSERT_EQ(vban::VBAN_EXTENDED_SR_COUNT, 22U);
    ASSERT_EQ(vban::VBAN_SR_TABLE[21], 768000U);

    // Helper functions
    auto rate21 = vban::getSampleRate(21);
    ASSERT_TRUE(rate21.has_value());
    ASSERT_EQ(*rate21, 768000U);

    // Out of range indices
    auto rate22 = vban::getSampleRate(22);
    ASSERT_FALSE(rate22.has_value());

    auto rate31 = vban::getSampleRate(31);
    ASSERT_FALSE(rate31.has_value());
}

// ==============================================================================
// 3. Data Types & Sample Sizes
// ==============================================================================

TEST_CASE("vban_data_types_and_sample_sizes") {
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Byte8), 1U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Int16), 2U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Int24), 3U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Int32), 4U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Float32), 4U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Float64), 8U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Bits12), 2U);
    ASSERT_EQ(vban::getSampleSize(vban::VBanDataType::Bits10), 2U);
}

TEST_CASE("vban_payload_calculation") {
    // 1 sample, 1 ch, 16-bit
    ASSERT_EQ(vban::calculatePayloadBytes(0, 0, 2), 2U);
    ASSERT_EQ(vban::calculateTotalPacketBytes(0, 0, 2), 30U);

    // 256 samples, 2 ch, 16-bit
    ASSERT_EQ(vban::calculatePayloadBytes(255, 1, 2), 1024U);
    ASSERT_EQ(vban::calculateTotalPacketBytes(255, 1, 2), 1052U);

    // 128 samples, 8 ch, 32-bit
    ASSERT_EQ(vban::calculatePayloadBytes(127, 7, 4), 4096U);
    ASSERT_EQ(vban::calculateTotalPacketBytes(127, 7, 4), 4124U);
}

// ==============================================================================
// 4. Stream Name Extraction & Trimming
// ==============================================================================

TEST_CASE("vban_stream_name_extraction") {
    char name1[16] = {'M', 'a', 'i', 'n', '\0', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_EQ(vban::extractStreamName(name1), "Main");

    char name2[16] = {'S', 't', 'r', 'e', 'a', 'm', '1', ' ', ' ', ' ', 0, 0, 0, 0, 0, 0};
    ASSERT_EQ(vban::extractStreamName(name2), "Stream1");

    char name3[16] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6'};
    ASSERT_EQ(vban::extractStreamName(name3), "1234567890123456");
}

// ==============================================================================
// 5. 10-Step Validation Pipeline Tests
// ==============================================================================

TEST_CASE("vban_parser_step1_packet_too_small") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    ASSERT_EQ(parser.parse(nullptr, 0, out), vban::ParseResult::PacketTooSmall);

    uint8_t tiny[27] = {0};
    ASSERT_EQ(parser.parse(tiny, sizeof(tiny), out), vban::ParseResult::PacketTooSmall);
}

TEST_CASE("vban_parser_step2_invalid_magic") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    hdr->magic = 0x54534554U; // 'TEST'

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::InvalidMagic);
}

TEST_CASE("vban_parser_step3_unsupported_subprotocol") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // Serial subprotocol (0x20)
    hdr->format_SR = static_cast<uint8_t>(vban::VBanSubProtocol::Serial) | 0x03;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::UnsupportedSubprotocol);

    // Text subprotocol (0x40)
    hdr->format_SR = static_cast<uint8_t>(vban::VBanSubProtocol::Text) | 0x03;
    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::UnsupportedSubprotocol);
}

TEST_CASE("vban_parser_step4_unsupported_codec") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // VBCA compressed codec (0x10)
    hdr->format_bit = static_cast<uint8_t>(vban::VBanCodec::VBCA) | 0x01;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::UnsupportedCodec);
}

TEST_CASE("vban_parser_step5_reserved_bit3_check") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // Set Bit 3 (0x08)
    hdr->format_bit |= vban::VBAN_RESERVED_BIT3_MASK;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::ReservedBitSet);
}

TEST_CASE("vban_parser_step6_invalid_sample_rate") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket();
    auto* hdr = reinterpret_cast<vban::VBanHeader*>(packet.data());
    // SR Index 22 (invalid)
    hdr->format_SR = (hdr->format_SR & 0xE0) | 22;

    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::InvalidSampleRate);
}

TEST_CASE("vban_parser_step7_unsupported_data_type") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Byte8 (0x00)
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Byte8);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::UnsupportedDataType);

    // Float64 (0x05)
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Float64);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::UnsupportedDataType);
}

TEST_CASE("vban_parser_step8_stream_name_filtering") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Matching stream
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1");
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::Success);

    // Case-insensitive match ("stream1" matches "Stream1")
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "stream1");
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::Success);

    // Mismatched stream
    auto p3 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream2");
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::StreamNameMismatch);

    // Wildcard mode (empty target accepts everything)
    parser.setTargetStreamName("");
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::Success);
}

TEST_CASE("vban_parser_step9_payload_size_mismatch") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16); // Expected 1052 bytes
    ASSERT_EQ(packet.size(), 1052U);

    // Truncated packet (-1 byte)
    ASSERT_EQ(parser.parse(packet.data(), packet.size() - 1, out), vban::ParseResult::PayloadSizeMismatch);

    // Packet with trailing garbage (+4 bytes)
    packet.resize(packet.size() + 4);
    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::PayloadSizeMismatch);
}

TEST_CASE("vban_parser_valid_packet_and_metadata") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    auto packet = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 42);
    ASSERT_EQ(parser.parse(packet.data(), packet.size(), out), vban::ParseResult::Success);

    ASSERT_EQ(out.sampleRate, 48000U);
    ASSERT_EQ(out.channels, 2U);
    ASSERT_EQ(out.samplesPerFrame, 256U);
    ASSERT_EQ(out.sampleSize, 2U);
    ASSERT_EQ(out.payloadBytes, 1024U);
    ASSERT_EQ(out.dataType, vban::VBanDataType::Int16);
    ASSERT_EQ(out.streamName, "Stream1");
    ASSERT_EQ(out.frameNumber, 42U);
    ASSERT_EQ(out.payload, packet.data() + 28);
}

// ==============================================================================
// 6. Sequence Number Loss Tracking & Wrap Tests
// ==============================================================================

TEST_CASE("vban_parser_sequence_loss_tracking") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Packet 0: frame 100
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 100);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Packet 1: frame 101 (delta 1, no loss)
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 101);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Packet 2: frame 105 (delta 4, 3 lost)
    auto p3 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 105);
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 3U);

    // Packet 3: frame 103 (out of order)
    auto p4 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 103);
    ASSERT_EQ(parser.parse(p4.data(), p4.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 3U);
    ASSERT_EQ(parser.getOutOfOrderPackets(), 1U);
}

TEST_CASE("vban_parser_sequence_wrap_around") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // Frame 0xFFFFFFFF
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0xFFFFFFFFU);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Frame 0x00000000 (wraps over, delta == 1)
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0x00000000U);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 0U);

    // Frame 0x00000005 (delta == 5, 4 lost)
    auto p3 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0x00000005U);
    ASSERT_EQ(parser.parse(p3.data(), p3.size(), out), vban::ParseResult::Success);
    ASSERT_EQ(parser.getLostPackets(), 4U);
}

// ==============================================================================
// 7. RFC 3550 Inter-Arrival Jitter Calculation
// ==============================================================================

TEST_CASE("vban_parser_rfc3550_jitter_ideal_arrival") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    // 48 kHz, 256 samples/packet -> nominal interval = 256 / 48000 = 5.333333 ms
    constexpr uint64_t intervalNs = 5333333ULL;
    uint64_t timeNs = 1000000000ULL;

    for (uint32_t frame = 0; frame < 10; ++frame) {
        auto p = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", frame);
        ASSERT_EQ(parser.parse(p.data(), p.size(), timeNs, out), vban::ParseResult::Success);
        timeNs += intervalNs;
    }

    // Jitter with perfect arrivals must remain close to 0
    ASSERT_NEAR(parser.getJitterMs(), 0.0, 0.001);
}

TEST_CASE("vban_parser_rfc3550_jitter_filter_convergence") {
    vban::VbanParser parser("Stream1");
    vban::ParsedPacket out;

    constexpr uint64_t nominalNs = 5333333ULL;
    uint64_t timeNs = 1000000000ULL;

    // Packet 0
    auto p0 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 0);
    ASSERT_EQ(parser.parse(p0.data(), p0.size(), timeNs, out), vban::ParseResult::Success);
    ASSERT_NEAR(parser.getJitterMs(), 0.0, 1e-6);

    // Packet 1 delayed by 2.0 ms (+2,000,000 ns)
    timeNs += nominalNs + 2000000ULL;
    auto p1 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 1);
    ASSERT_EQ(parser.parse(p1.data(), p1.size(), timeNs, out), vban::ParseResult::Success);

    // D = 2.0 ms. Jitter = 0 + (2.0 - 0) / 16.0 = 0.125 ms
    ASSERT_NEAR(parser.getJitterMs(), 0.125, 0.001);

    // Packet 2 arrives on schedule (+nominalNs)
    timeNs += nominalNs;
    auto p2 = createVbanPacket(3, 255, 1, vban::VBanDataType::Int16, "Stream1", 2);
    ASSERT_EQ(parser.parse(p2.data(), p2.size(), timeNs, out), vban::ParseResult::Success);

    // D = 0.0 ms. Jitter = 0.125 + (0.0 - 0.125) / 16.0 = 0.1171875 ms
    ASSERT_NEAR(parser.getJitterMs(), 0.1171875, 0.001);
}
```

---

## 5. Architectural Synthesis & Integration Strategy

### 5.1 Interfacing with Milestone 2 Downstream Modules

1. **`vban_converter.hpp` / `vban_converter.cpp`:**
   Once `VbanParser::parse(...)` returns `ParseResult::Success`, the caller receives `ParsedPacket`. The payload pointer (`outPacket.payload`), sample count (`outPacket.samplesPerFrame * outPacket.channels`), and data type (`outPacket.dataType`) are forwarded directly to the dequantizer routines (`convertInt16ToFloat`, `convertInt24ToFloat`, `convertInt32ToFloat`, `convertFloat32Sanitized`).
2. **`spsc_ring_buffer.hpp`:**
   Dequantized interleaved Float32 audio samples are written into the $2^{18}$-frame lock-free SPSC buffer via `ringBuffer.write(floatSamples, frameCount)`.
3. **`NativeTelemetry` in `vban_engine.cpp`:**
   The parser's sequence and jitter metrics (`lostPackets`, `jitterMs`) feed directly into `telemetryCache_` in `VbanEngine` for consumption by the Kotlin Jetpack Compose dashboard via JNI.

---

## 6. Verification Method

To verify these code templates independently when applied:
1. Copy the code templates into:
   - `app/src/main/cpp/include/vban_protocol.hpp`
   - `app/src/main/cpp/include/vban_parser.hpp`
   - `app/src/main/cpp/src/vban_parser.cpp`
   - `native_tests/test_vban_protocol.cpp`
2. Run CTest on host:
   ```powershell
   cmake --build native_tests/build
   ctest --test-dir native_tests/build --output-on-failure
   ```
3. Run Gradle assemble on Android:
   ```powershell
   .\gradlew.bat assembleDebug
   ```
