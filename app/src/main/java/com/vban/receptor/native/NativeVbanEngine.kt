package com.vban.receptor.native

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Immutable telemetry snapshot received from the native C++ VBAN engine.
 *
 * Parameters match exact JNI constructor signature: `(IIIJJFFFFFJJ)V`
 *
 * @property sampleRate Active sample rate in Hz (e.g. 48000, 96000, 192000, 768000).
 * @property channelCount Active channel count (1 to 8).
 * @property bitDepth PCM bit depth (16, 24, 32).
 * @property totalPacketsReceived Monotonic count of valid received UDP packets.
 * @property lostPackets Monotonic count of missing packets detected via sequence gap analysis.
 * @property jitterMs Current inter-arrival network jitter (RFC 3550 EWMA) in milliseconds.
 * @property bufferOccupancyPercent SPSC circular ring buffer watermark percentage [0.0f, 100.0f].
 * @property peakLevelLeft Linear audio peak amplitude for Left channel [0.0f, 1.0f].
 * @property peakLevelRight Linear audio peak amplitude for Right channel [0.0f, 1.0f].
 * @property targetWatermarkMs Dynamic adaptive jitter target watermark in milliseconds.
 * @property unrecoverableDrops Monotonic count of packets dropped or unrecoverable gaps.
 * @property glitchCount Monotonic count of buffer underrun/glitch events.
 */
data class NativeTelemetry(
    val sampleRate: Int = 0,
    val channelCount: Int = 0,
    val bitDepth: Int = 0,
    val totalPacketsReceived: Long = 0L,
    val lostPackets: Long = 0L,
    val jitterMs: Float = 0.0f,
    val bufferOccupancyPercent: Float = 0.0f,
    val peakLevelLeft: Float = 0.0f,
    val peakLevelRight: Float = 0.0f,
    val targetWatermarkMs: Float = 0.0f,
    val unrecoverableDrops: Long = 0L,
    val glitchCount: Long = 0L
) {
    /**
     * Computed packet loss rate percentage [0.0%, 100.0%].
     */
    val packetLossRatePercent: Float
        get() {
            val total = totalPacketsReceived + lostPackets
            return if (total > 0L) {
                (lostPackets.toFloat() / total.toFloat()) * 100.0f
            } else {
                0.0f
            }
        }

    /**
     * Left channel peak converted to decibels relative to Full Scale (dBFS).
     * Clamped to standard metering range [-60.0f, 0.0f].
     */
    val peakLevelLeftDbfs: Float
        get() = linearToDbfs(peakLevelLeft)

    /**
     * Right channel peak converted to decibels relative to Full Scale (dBFS).
     * Clamped to standard metering range [-60.0f, 0.0f].
     */
    val peakLevelRightDbfs: Float
        get() = linearToDbfs(peakLevelRight)

    companion object {
        /**
         * Converts linear amplitude in [0.0, 1.0] to decibels Full Scale (dBFS).
         */
        fun linearToDbfs(linear: Float): Float {
            if (linear <= 0.00001f) return -60.0f
            val db = 20.0f * kotlin.math.log10(linear)
            return db.coerceIn(-60.0f, 0.0f)
        }
    }
}

/**
 * Thread-safe singleton managing the native VBAN receptor engine lifecycle,
 * JNI communication, and reactive telemetry streaming.
 */
object NativeVbanEngine {
    private const val TAG = "NativeVbanEngine"
    private const val LIB_NAME = "vban_engine"

    private val isLibraryLoaded = AtomicBoolean(false)
    private val isInitialized = AtomicBoolean(false)

    private val _isRunning = MutableStateFlow(false)
    val isRunning: StateFlow<Boolean> = _isRunning.asStateFlow()

    private val _telemetry = MutableStateFlow(NativeTelemetry())
    val telemetry: StateFlow<NativeTelemetry> = _telemetry.asStateFlow()

    private val _latencyProfile = MutableStateFlow(com.vban.receptor.audio.LatencyProfile.BALANCED)
    val latencyProfile: StateFlow<com.vban.receptor.audio.LatencyProfile> = _latencyProfile.asStateFlow()

    private val _spectrumBands = MutableStateFlow(FloatArray(64))
    val spectrumBands: StateFlow<FloatArray> = _spectrumBands.asStateFlow()

    private var telemetryPollingJob: Job? = null
    private val scope = CoroutineScope(Dispatchers.Default)

    init {
        try {
            System.loadLibrary(LIB_NAME)
            isLibraryLoaded.set(true)
            Log.i(TAG, "Native library '$LIB_NAME' successfully loaded.")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load native library '$LIB_NAME': ${e.message}", e)
        }
    }

    // =========================================================================
    // JNI External Declarations
    // =========================================================================

    @JvmStatic
    private external fun initEngine(): Boolean

    @JvmStatic
    private external fun startReceiver(
        listenIp: String,
        port: Int,
        streamName: String,
        deviceId: Int
    ): Boolean

    @JvmStatic
    private external fun stopReceiver()

    @JvmStatic
    private external fun setAudioDevice(deviceId: Int)

    @JvmStatic
    private external fun getTelemetry(): NativeTelemetry?

    @JvmStatic
    private external fun setTargetLatencyMs(targetMs: Float): Boolean

    @JvmStatic
    private external fun getSpectrumBands(outArray: FloatArray): Boolean

    // =========================================================================
    // Public API
    // =========================================================================

    /**
     * Initializes the native C++20 engine environment.
     * Safe to invoke multiple times.
     */
    @Synchronized
    fun initialize(): Boolean {
        if (!isLibraryLoaded.get()) {
            Log.e(TAG, "Cannot initialize: '$LIB_NAME' library is not loaded.")
            return false
        }
        if (isInitialized.get()) {
            return true
        }

        val success = initEngine()
        if (success) {
            isInitialized.set(true)
            Log.i(TAG, "Native VBAN Engine initialized successfully.")
        } else {
            Log.e(TAG, "Native VBAN Engine initialization failed.")
        }
        return success
    }

    /**
     * Starts receiving VBAN packets and launches Oboe audio playback.
     *
     * @param listenIp Local network IP or multicast address (e.g. "0.0.0.0" or "239.255.0.1").
     * @param port UDP port (VBAN default is 6980).
     * @param streamName Stream name filter (matching VBAN packet header).
     * @param deviceId Android AudioDeviceInfo id (0 = default device).
     * @param pollIntervalMs Telemetry polling frequency in milliseconds (default: 33ms ~ 30 FPS).
     */
    @Synchronized
    fun start(
        listenIp: String = "0.0.0.0",
        port: Int = 6980,
        streamName: String = "",
        deviceId: Int = 0,
        pollIntervalMs: Long = 16L
    ): Boolean {
        if (!initialize()) {
            return false
        }
        if (_isRunning.value) {
            Log.w(TAG, "Engine is already running.")
            return true
        }

        val started = startReceiver(listenIp, port, streamName, deviceId)
        if (started) {
            _isRunning.value = true
            setTargetLatencyMs(_latencyProfile.value.targetLatencyMs)
            startTelemetryPolling(pollIntervalMs)
            Log.i(TAG, "VBAN Receiver started on $listenIp:$port, stream='$streamName', device=$deviceId")
        } else {
            Log.e(TAG, "Failed to start VBAN Receiver.")
        }
        return started
    }

    /**
     * Stops the VBAN reception and audio playback pipeline, canceling the polling job.
     */
    @Synchronized
    fun stop() {
        if (!_isRunning.value) {
            return
        }
        stopTelemetryPolling()
        if (isLibraryLoaded.get()) {
            stopReceiver()
        }
        _isRunning.value = false
        _telemetry.value = NativeTelemetry()
        Log.i(TAG, "VBAN Receiver stopped.")
    }

    /**
     * Dynamically switches the active output audio device (e.g., when a USB DAC is attached).
     */
    fun setDevice(deviceId: Int) {
        if (isLibraryLoaded.get()) {
            setAudioDevice(deviceId)
            Log.i(TAG, "Audio device set to ID $deviceId")
        }
    }

    /**
     * Sets target latency profile (Ultra-Low, Balanced, Anti-Jitter).
     */
    fun setLatencyProfile(profile: com.vban.receptor.audio.LatencyProfile) {
        _latencyProfile.value = profile
        if (isLibraryLoaded.get()) {
            setTargetLatencyMs(profile.targetLatencyMs)
            Log.i(TAG, "Latency profile set to ${profile.displayName} (${profile.targetLatencyMs} ms)")
        }
    }

    /**
     * Synchronously queries a single telemetry snapshot without blocking.
     */
    fun pollTelemetrySnapshot(): NativeTelemetry {
        if (!isLibraryLoaded.get() || !_isRunning.value) {
            return _telemetry.value
        }
        return getTelemetry() ?: _telemetry.value
    }

    // =========================================================================
    // Internal Telemetry Poller
    // =========================================================================

    private fun startTelemetryPolling(pollIntervalMs: Long) {
        telemetryPollingJob?.cancel()
        val spectrumBuffer = FloatArray(64)
        telemetryPollingJob = scope.launch {
            while (isActive && _isRunning.value) {
                try {
                    val snapshot = getTelemetry()
                    if (snapshot != null) {
                        _telemetry.value = snapshot
                    }
                    if (getSpectrumBands(spectrumBuffer)) {
                        _spectrumBands.value = spectrumBuffer.copyOf()
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Error polling telemetry: ${e.message}")
                }
                delay(pollIntervalMs)
            }
        }
    }

    private fun stopTelemetryPolling() {
        telemetryPollingJob?.cancel()
        telemetryPollingJob = null
    }
}
