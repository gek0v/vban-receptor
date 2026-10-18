package com.vban.receptor.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioMixerAttributes
import android.os.Build
import android.util.Log
import androidx.annotation.RequiresApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.Executors

/**
 * Sealed hierarchy representing the active state of bit-perfect audio output.
 */
sealed class BitPerfectState {
    /**
     * Bit-perfect mode is inactive (streaming stopped or routed to non-bit-perfect device).
     */
    data object Inactive : BitPerfectState()

    /**
     * Bit-perfect bypass is fully engaged on Android 14+.
     * System volume scaling and resampling are completely bypassed.
     */
    data class Active(
        val deviceId: Int,
        val deviceName: String,
        val sampleRate: Int,
        val channelCount: Int,
        val encoding: Int
    ) : BitPerfectState() {
        val summary: String
            get() = "Bit-Perfect Active: ${sampleRate}Hz | ${channelCount}ch | System Volume Bypassed"
    }

    /**
     * Bit-perfect output could not be engaged; system operates in graceful fallback.
     */
    data class Fallback(
        val reason: FallbackReason,
        val message: String
    ) : BitPerfectState()
}

/**
 * Reasons why bit-perfect mode could not be enabled.
 */
enum class FallbackReason {
    /** OS version is Android 13 or lower (API < 34). */
    OS_VERSION_BELOW_34,

    /** Selected device is not a USB audio peripheral. */
    DEVICE_NOT_USB,

    /** Device audio HAL does not support MIXER_BEHAVIOR_BIT_PERFECT. */
    HAL_DOES_NOT_SUPPORT_BIT_PERFECT,

    /** Target sample rate is not supported by the DAC hardware. */
    SAMPLE_RATE_UNSUPPORTED,

    /** Framework rejected setPreferredMixerAttributes call. */
    FRAMEWORK_SET_FAILED,

    /** Active device was unplugged or disconnected. */
    DEVICE_DISCONNECTED
}

/**
 * Controller managing Android 14+ Bit-Perfect USB DAC audio output.
 *
 * Implements:
 * - Hardware query via `AudioManager.getSupportedMixerAttributes`.
 * - Negotiation of `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT`.
 * - Bit-perfect activation via `AudioManager.setPreferredMixerAttributes`.
 * - Dynamic listener for system-level mixer attribute revocations.
 * - Complete teardown and restoration via `AudioManager.clearPreferredMixerAttributes`.
 * - Graceful fallback paths for Android 13- and non-bit-perfect HALs.
 */
class BitPerfectController(
    private val context: Context
) {
    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val listenerExecutor = Executors.newSingleThreadExecutor()

    private val _state = MutableStateFlow<BitPerfectState>(BitPerfectState.Inactive)
    val state: StateFlow<BitPerfectState> = _state.asStateFlow()

    private var activeDevice: AudioDeviceInfo? = null
    private var activeAttributes: AudioAttributes? = null

    // Android 14+ Mixer Attributes Change Listener
    private val mixerListener: Any? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
        AudioManager.OnPreferredMixerAttributesChangedListener { attributes, device, mixerAttributes ->
            Log.i(TAG, "onPreferredMixerAttributesChanged: device=${device.id}, behavior=${mixerAttributes?.mixerBehavior}")
            if (mixerAttributes == null || mixerAttributes.mixerBehavior != AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT) {
                if (_state.value is BitPerfectState.Active) {
                    Log.w(TAG, "Bit-perfect mode was overridden or cleared by system.")
                    _state.value = BitPerfectState.Fallback(
                        reason = FallbackReason.FRAMEWORK_SET_FAILED,
                        message = "System reset mixer attributes to default behavior."
                    )
                }
            }
        }
    } else {
        null
    }

    init {
        registerMixerListenerIfSupported()
    }

    /**
     * Checks whether the given AudioDeviceInfo natively supports BIT_PERFECT mixer mode.
     */
    fun isBitPerfectSupported(device: AudioDeviceInfo): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            return false // Bit-perfect API requires Android 14+ (API 34)
        }

        // Bit-perfect is strictly defined for USB audio sinks
        if (device.type != AudioDeviceInfo.TYPE_USB_DEVICE &&
            device.type != AudioDeviceInfo.TYPE_USB_HEADSET &&
            device.type != AudioDeviceInfo.TYPE_USB_ACCESSORY
        ) {
            return false
        }

        return try {
            val supportedMixers = audioManager.getSupportedMixerAttributes(device)
            supportedMixers.any { it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT }
        } catch (e: Exception) {
            Log.w(TAG, "Error querying supported mixer attributes for device ${device.id}: ${e.message}")
            false
        }
    }

    /**
     * Attempts to configure bit-perfect output for the active audio stream.
     *
     * @param device Target AudioDeviceInfo.
     * @param sampleRate Incoming stream sample rate (Hz).
     * @param channelCount Channel count (1 for Mono, 2 for Stereo, up to 8).
     * @param encoding Audio encoding format (default: AudioFormat.ENCODING_PCM_FLOAT).
     * @return true if bit-perfect mode was successfully engaged, false otherwise.
     */
    @Synchronized
    fun enableBitPerfect(
        device: AudioDeviceInfo,
        sampleRate: Int,
        channelCount: Int,
        encoding: Int = AudioFormat.ENCODING_PCM_FLOAT
    ): Boolean {
        Log.i(TAG, "Requesting Bit-Perfect output: device=${device.id}, rate=${sampleRate}Hz, ch=$channelCount")

        // 1. Android version check
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            Log.i(TAG, "Android version is ${Build.VERSION.SDK_INT} (< 34). Using AAudio Exclusive fallback.")
            _state.value = BitPerfectState.Fallback(
                reason = FallbackReason.OS_VERSION_BELOW_34,
                message = "Android 13 or lower: using AAudio Exclusive mode fallback."
            )
            return false
        }

        // 2. Peripheral type validation
        if (device.type != AudioDeviceInfo.TYPE_USB_DEVICE &&
            device.type != AudioDeviceInfo.TYPE_USB_HEADSET &&
            device.type != AudioDeviceInfo.TYPE_USB_ACCESSORY
        ) {
            Log.i(TAG, "Device ${device.id} is not a USB output. Operating in standard mode.")
            _state.value = BitPerfectState.Fallback(
                reason = FallbackReason.DEVICE_NOT_USB,
                message = "Device is not a USB DAC: operating in standard mixer mode."
            )
            return false
        }

        return applyBitPerfectAndroid14(device, sampleRate, channelCount, encoding)
    }

    @RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
    private fun applyBitPerfectAndroid14(
        device: AudioDeviceInfo,
        sampleRate: Int,
        channelCount: Int,
        encoding: Int
    ): Boolean {
        try {
            val supportedMixers = audioManager.getSupportedMixerAttributes(device)
            val bitPerfectMixers = supportedMixers.filter {
                it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT
            }

            if (bitPerfectMixers.isEmpty()) {
                Log.w(TAG, "Device ${device.id} HAL does not report MIXER_BEHAVIOR_BIT_PERFECT.")
                _state.value = BitPerfectState.Fallback(
                    reason = FallbackReason.HAL_DOES_NOT_SUPPORT_BIT_PERFECT,
                    message = "USB DAC HAL does not support bit-perfect bypass. Falling back to AAudio exclusive."
                )
                return false
            }

            // Standard Media AudioAttributes matching Oboe AAudio stream
            val audioAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()

            // Strategy 1: Find pre-configured bit-perfect attribute matching exact sample rate & channels
            var targetMixer = bitPerfectMixers.firstOrNull { mixer ->
                mixer.format.sampleRate == sampleRate && mixer.format.channelCount == channelCount
            }

            // Strategy 2: If no exact match in enumerations, build custom bit-perfect attribute
            if (targetMixer == null) {
                Log.d(TAG, "No exact match in enumerated mixers. Building custom bit-perfect mixer for ${sampleRate}Hz...")
                val channelMask = when (channelCount) {
                    1 -> AudioFormat.CHANNEL_OUT_MONO
                    4 -> AudioFormat.CHANNEL_OUT_QUAD
                    6 -> AudioFormat.CHANNEL_OUT_5POINT1
                    8 -> AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
                    else -> AudioFormat.CHANNEL_OUT_STEREO
                }

                val customFormat = AudioFormat.Builder()
                    .setSampleRate(sampleRate)
                    .setChannelMask(channelMask)
                    .setEncoding(encoding)
                    .build()

                targetMixer = AudioMixerAttributes.Builder(customFormat)
                    .setMixerBehavior(AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT)
                    .build()
            }

            Log.i(TAG, "Applying setPreferredMixerAttributes: format=${targetMixer.format}")
            val success = audioManager.setPreferredMixerAttributes(audioAttributes, device, targetMixer)

            if (success) {
                activeDevice = device
                activeAttributes = audioAttributes
                val deviceName = device.productName?.toString() ?: "USB DAC (${device.id})"

                _state.value = BitPerfectState.Active(
                    deviceId = device.id,
                    deviceName = deviceName,
                    sampleRate = sampleRate,
                    channelCount = channelCount,
                    encoding = encoding
                )
                Log.i(TAG, "Bit-perfect mode successfully configured on device ${device.id}!")
                return true
            } else {
                Log.e(TAG, "setPreferredMixerAttributes returned false.")
                _state.value = BitPerfectState.Fallback(
                    reason = FallbackReason.FRAMEWORK_SET_FAILED,
                    message = "Audio policy rejected preferred mixer attributes."
                )
                return false
            }
        } catch (e: Exception) {
            Log.e(TAG, "Exception during applyBitPerfect: ${e.message}", e)
            _state.value = BitPerfectState.Fallback(
                reason = FallbackReason.FRAMEWORK_SET_FAILED,
                message = "Exception setting mixer attributes: ${e.message}"
            )
            return false
        }
    }

    /**
     * Clears preferred mixer attributes and restores normal system mixer behavior.
     * Safe to invoke idempotently on stream stop or disconnect.
     */
    @Synchronized
    fun disableBitPerfect() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val dev = activeDevice
            val attr = activeAttributes
            if (dev != null && attr != null) {
                try {
                    Log.i(TAG, "Clearing preferred mixer attributes for device ${dev.id}...")
                    val cleared = audioManager.clearPreferredMixerAttributes(attr, dev)
                    Log.i(TAG, "clearPreferredMixerAttributes result: $cleared")
                } catch (e: Exception) {
                    Log.w(TAG, "Error clearing mixer attributes: ${e.message}")
                }
            }
        }

        activeDevice = null
        activeAttributes = null
        _state.value = BitPerfectState.Inactive
    }

    /**
     * Cleans up background executors and unregisters listeners.
     */
    fun release() {
        disableBitPerfect()
        unregisterMixerListenerIfSupported()
        listenerExecutor.shutdown()
    }

    private fun registerMixerListenerIfSupported() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && mixerListener != null) {
            try {
                audioManager.addOnPreferredMixerAttributesChangedListener(
                    listenerExecutor,
                    mixerListener as AudioManager.OnPreferredMixerAttributesChangedListener
                )
                Log.d(TAG, "OnPreferredMixerAttributesChangedListener registered.")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to register preferred mixer attributes listener: ${e.message}")
            }
        }
    }

    private fun unregisterMixerListenerIfSupported() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE && mixerListener != null) {
            try {
                audioManager.removeOnPreferredMixerAttributesChangedListener(
                    mixerListener as AudioManager.OnPreferredMixerAttributesChangedListener
                )
                Log.d(TAG, "OnPreferredMixerAttributesChangedListener unregistered.")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to unregister preferred mixer attributes listener: ${e.message}")
            }
        }
    }

    companion object {
        private const val TAG = "BitPerfectController"
    }
}
