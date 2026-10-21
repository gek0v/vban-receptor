package com.vban.receptor.audio

/**
 * Adaptive Latency and Jitter Buffer Profiles for VBAN audio reception.
 *
 * Defines target buffer watermark, drift compensation PID constants,
 * and filter time constants tailored for varying network environments.
 *
 * @property id Unique identifier string for serialization and settings persistence.
 * @property displayName Human-readable profile title for UI display.
 * @property subtitle Short description explaining the optimal network condition.
 * @property targetLatencyMsMin Lower bound of target buffer latency in milliseconds.
 * @property targetLatencyMsMax Upper bound of target buffer latency in milliseconds.
 */
enum class LatencyProfile(
    val id: String,
    val displayName: String,
    val subtitle: String,
    val targetLatencyMsMin: Float,
    val targetLatencyMsMax: Float
) {
    /**
     * AUTO Profile (Default):
     * Target latency 5–25 ms.
     * Dynamic RFC 3550 servo jitter buffer adaptation.
     */
    AUTO(
        id = "AUTO",
        displayName = "Auto (Adaptativo)",
        subtitle = "5–25 ms · RFC 3550 Servo",
        targetLatencyMsMin = 5.0f,
        targetLatencyMsMax = 25.0f
    ),

    /**
     * ULTRA_LOW Profile:
     * Target latency 5–10 ms (~480–960 samples at 48 kHz).
     * Designed for wired Ethernet, direct Thunderbolt/USB networking, or dedicated 5 GHz / 6 GHz Wi-Fi links.
     */
    ULTRA_LOW(
        id = "ULTRA_LOW",
        displayName = "Ultra-Low",
        subtitle = "5–10 ms · Wired / 5GHz Wi-Fi",
        targetLatencyMsMin = 5.0f,
        targetLatencyMsMax = 10.0f
    ),

    /**
     * BALANCED Profile (Default):
     * Target latency 25–40 ms (~1200–1920 samples at 48 kHz).
     * Optimal balance between low latency and glitch-free playback on typical home or studio Wi-Fi.
     */
    BALANCED(
        id = "BALANCED",
        displayName = "Balanced",
        subtitle = "25–40 ms · Standard Wi-Fi",
        targetLatencyMsMin = 25.0f,
        targetLatencyMsMax = 40.0f
    ),

    /**
     * ANTI_JITTER Profile:
     * Target latency 100–200 ms (~4800–9600 samples at 48 kHz).
     * Maximum jitter resilience for congested networks, long-range 2.4 GHz Wi-Fi, or lossy RF links.
     */
    ANTI_JITTER(
        id = "ANTI_JITTER",
        displayName = "Anti-Jitter",
        subtitle = "100–200 ms · Congested / 2.4GHz",
        targetLatencyMsMin = 100.0f,
        targetLatencyMsMax = 200.0f
    );

    /**
     * Nominal midpoint target latency in milliseconds.
     * For AUTO, defaults to targetLatencyMsMin (5 ms base latency) to let the RFC 3550 servo adapt upwards.
     */
    val targetLatencyMs: Float
        get() = if (this == AUTO) targetLatencyMsMin else (targetLatencyMsMin + targetLatencyMsMax) * 0.5f

    /**
     * Human-readable formatted latency range, e.g. "5–10 ms".
     */
    val latencyRangeString: String
        get() = "${targetLatencyMsMin.toInt()}–${targetLatencyMsMax.toInt()} ms"

    companion object {
        /**
         * Default latency profile recommended for general streaming.
         */
        val DEFAULT: LatencyProfile = AUTO

        /**
         * Parses a profile by its ID, falling back to [DEFAULT] if not found.
         */
        fun fromId(id: String?): LatencyProfile {
            if (id == null) return DEFAULT
            return entries.firstOrNull { it.id.equals(id, ignoreCase = true) } ?: DEFAULT
        }
    }
}
