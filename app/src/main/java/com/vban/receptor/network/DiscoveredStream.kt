package com.vban.receptor.network

import java.util.Locale

/**
 * Immutable data model representing a VBAN audio stream discovered on the local network.
 *
 * @property streamName Name of the stream parsed from the 16-byte VBAN header field.
 * @property sourceIp IPv4 address of the sender machine broadcasting VBAN UDP packets.
 * @property port Destination UDP port (VBAN standard is 6980).
 * @property sampleRate Audio sample rate in Hz (e.g., 44100, 48000, 96000, 192000).
 * @property channelCount Number of audio channels (e.g., 1 for Mono, 2 for Stereo).
 * @property bitDepth PCM/Audio bit depth resolution (e.g., 16, 24, 32).
 * @property lastSeenMs Monotonic system timestamp in milliseconds when this stream was last detected.
 */
data class DiscoveredStream(
    val streamName: String,
    val sourceIp: String,
    val port: Int,
    val sampleRate: Int,
    val channelCount: Int,
    val bitDepth: Int,
    val lastSeenMs: Long = System.currentTimeMillis()
) {
    /**
     * Unique identifier composed of IP, port, and stream name to differentiate
     * multiple concurrent streams across network hosts.
     */
    val id: String
        get() = "$sourceIp:$port:$streamName"

    /**
     * Human-readable channel topology description (e.g., "Mono", "Stereo", "5.1 Surround").
     */
    val channelDescription: String
        get() = when (channelCount) {
            1 -> "Mono"
            2 -> "Stereo"
            4 -> "Quad"
            6 -> "5.1 Surround"
            8 -> "7.1 Surround"
            else -> "$channelCount ch"
        }

    /**
     * Formatted sample rate string (e.g., "44.1 kHz", "48 kHz", "96 kHz", "192 kHz").
     */
    val formattedSampleRate: String
        get() = if (sampleRate % 1000 == 0) {
            "${sampleRate / 1000} kHz"
        } else {
            val khz = sampleRate / 1000.0
            if (khz == khz.toLong().toDouble()) {
                "${khz.toLong()} kHz"
            } else {
                String.format(Locale.US, "%.1f kHz", khz)
            }
        }

    /**
     * Audio format badge string combining sample rate, bit depth, and channel layout.
     * Example: "96 kHz | 24-bit | Stereo"
     */
    val formatBadge: String
        get() = "$formattedSampleRate | $bitDepth-bit | $channelDescription"
}
