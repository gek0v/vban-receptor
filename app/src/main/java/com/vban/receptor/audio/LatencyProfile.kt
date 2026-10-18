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
 * @property targetSamplesAt48k Nominal target buffer occupancy in audio frames at 48 kHz.
 * @property targetOccupancyPercent Nominal target buffer occupancy percentage [0.0%, 100.0%].
 * @property emaTauSeconds EMA filter time constant in seconds for clock drift smoothing.
 * @property kp Proportional gain for clock drift compensation (PPM / % error).
 * @property ki Integral gain for steady-state clock drift compensation (PPM / (% error * sec)).
 * @property maxDriftPpm Maximum clock slew clamp (+- PPM).
 */
enum class LatencyProfile(
    val id: String,
    val displayName: String,
    val subtitle: String,
    val targetLatencyMsMin: Float,
    val targetLatencyMsMax: Float,
    val targetSamplesAt48k: Int,
    val targetOccupancyPercent: Float,
    val emaTauSeconds: Double,
    val kp: Double,
    val ki: Double,
    val maxDriftPpm: Double
) {
    /**
     * ULTRA_LOW Profile:
     * Target latency 5–10 ms (~480–960 samples at 48 kHz).
     * Designed for wired Ethernet, direct Thunderbolt/USB networking, or dedicated 5 GHz / 6 GHz Wi-Fi links.
     * Uses fast EMA filter (tau ~2.0s) and aggressive proportional tracking (kp ~4.0) to minimize delay.
     */
    ULTRA_LOW(
        id = "ULTRA_LOW",
        displayName = "Ultra-Low",
        subtitle = "5–10 ms · Wired / 5GHz Wi-Fi",
        targetLatencyMsMin = 5.0f,
        targetLatencyMsMax = 10.0f,
        targetSamplesAt48k = 480, // ~10 ms at 48 kHz
        targetOccupancyPercent = 15.0f,
        emaTauSeconds = 2.0,
        kp = 4.0,
        ki = 0.1,
        maxDriftPpm = 150.0
    ),

    /**
     * BALANCED Profile (Default):
     * Target latency 25–40 ms (~1200–1920 samples at 48 kHz).
     * Optimal balance between low latency and glitch-free playback on typical home or studio Wi-Fi.
     * Uses balanced EMA filter (tau ~8.0s) and moderate tracking (kp ~2.0).
     */
    BALANCED(
        id = "BALANCED",
        displayName = "Balanced",
        subtitle = "25–40 ms · Standard Wi-Fi",
        targetLatencyMsMin = 25.0f,
        targetLatencyMsMax = 40.0f,
        targetSamplesAt48k = 1920, // ~40 ms at 48 kHz
        targetOccupancyPercent = 35.0f,
        emaTauSeconds = 8.0,
        kp = 2.0,
        ki = 0.05,
        maxDriftPpm = 100.0
    ),

    /**
     * ANTI_JITTER Profile:
     * Target latency 100–200 ms (~4800–9600 samples at 48 kHz).
     * Maximum jitter resilience for congested networks, long-range 2.4 GHz Wi-Fi, or lossy RF links.
     * Uses heavy EMA filtering (tau ~15.0s) and gentle tracking (kp ~1.0) to completely prevent buffer under-runs.
     */
    ANTI_JITTER(
        id = "ANTI_JITTER",
        displayName = "Anti-Jitter",
        subtitle = "100–200 ms · Congested / 2.4GHz",
        targetLatencyMsMin = 100.0f,
        targetLatencyMsMax = 200.0f,
        targetSamplesAt48k = 4800, // ~100 ms at 48 kHz
        targetOccupancyPercent = 60.0f,
        emaTauSeconds = 15.0,
        kp = 1.0,
        ki = 0.02,
        maxDriftPpm = 80.0
    );

    /**
     * Nominal midpoint target latency in milliseconds.
     */
    val targetLatencyMs: Float
        get() = (targetLatencyMsMin + targetLatencyMsMax) * 0.5f

    /**
     * Human-readable formatted latency range, e.g. "5–10 ms".
     */
    val latencyRangeString: String
        get() = "${targetLatencyMsMin.toInt()}–${targetLatencyMsMax.toInt()} ms"

    companion object {
        /**
         * Default latency profile recommended for general audiophile streaming.
         */
        val DEFAULT: LatencyProfile = BALANCED

        /**
         * Parses a profile by its ID, falling back to [DEFAULT] if not found.
         */
        fun fromId(id: String?): LatencyProfile {
            if (id == null) return DEFAULT
            return entries.firstOrNull { it.id.equals(id, ignoreCase = true) } ?: DEFAULT
        }
    }
}
