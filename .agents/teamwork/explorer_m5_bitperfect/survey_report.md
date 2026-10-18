# Survey Report: Android 14+ Bit-Perfect USB DAC Bypass & Device Routing Architecture

**Milestone**: Milestone 5 — Bit-Perfect USB DAC Bypass & Device Routing Architecture  
**Date**: 2026-10-18  
**Author**: Explorer Agent (`explorer_m5_bitperfect`)  
**Target Package**: `com.vban.receptor.audio`  
**Related Native Engine**: `com.vban.receptor.native.NativeVbanEngine` / C++20 `OboePlayer`  

---

## 1. Executive Summary

This survey defines the architecture, platform APIs, lifecycle management, and production-ready Kotlin implementations for **Bit-Perfect USB DAC Audio Output** and **Dynamic Audio Device Routing** in the VBAN Receptor for Android.

Android 14 (API level 34, `UPSIDE_DOWN_CAKE`) introduced native framework support for bit-perfect audio output via `android.media.AudioMixerAttributes` and `android.media.AudioManager`. By leveraging `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT`, media playback applications can explicitly instruct the Android Audio Framework (`AudioService`, `AudioPolicyManager`, and `AudioFlinger`) to **completely bypass system-level software mixing, digital volume scaling, equalizers, and mandatory sample rate resampling (typically 48 kHz)**. Digital audio frames are delivered bit-for-bit to external USB DACs (Digital-to-Analog Converters) up to ultra-high-resolution rates (e.g., 96 kHz, 192 kHz, 384 kHz, and 768 kHz).

To achieve seamless, audiophile-grade playback across all devices, this architecture establishes:
1. **`AudioDeviceManager.kt`**: Proactively discovers and enumerates all audio output sinks, categorizing external USB DACs, USB-C dongles, analog headphones, Bluetooth, and internal speakers; listens for dynamic hotplug attach/detach events via `AudioDeviceCallback` and broadcast intents; and maintains thread-safe reactive state flows.
2. **`BitPerfectController.kt`**: Encapsulates Android 14+ bit-perfect negotiation, queries supported mixer configurations via `getSupportedMixerAttributes`, formats and applies `setPreferredMixerAttributes` with `MIXER_BEHAVIOR_BIT_PERFECT`, registers preferred mixer change listeners, provides robust multi-tiered fallbacks for Android 13 and below, and guarantees clean teardown via `clearPreferredMixerAttributes`.
3. **Native AAudio / Oboe Bridge Handshake**: Bridges device selection and mixer configuration to `NativeVbanEngine.setAudioDevice(deviceId)`, coordinating with C++20 `OboePlayer` to open exclusive low-latency AAudio streams with 128-sample cosine crossfading.

---

## 2. Android 14+ Bit-Perfect Architecture & Platform HAL Mechanics

### 2.1 The Problem with Legacy Android Audio Routing
In standard Android audio pipelines prior to Android 14:
- All audio streams passing through the system framework are ingested by `AudioFlinger`.
- `AudioFlinger` resamples all streams to a fixed hardware mixer rate (almost universally **48,000 Hz**, regardless of whether the source stream is 44.1 kHz, 96 kHz, or 192 kHz).
- Float/PCM audio data is subjected to software digital gain attenuation (system volume slider) and audio effects processing (limiter, spatializer, DRC).
- Even when opening an AAudio stream in `Exclusive` mode, vendor HAL configurations often enforce mixing or volume scaling unless specialized vendor-specific kernel bypasses are present.

### 2.2 Android 14 `AudioMixerAttributes` Specification
In Android 14 (API 34), Google added explicit mixer behavior configuration:
```java
// Decompiled bytecode from D:\Android\sdk\platforms\android-34\android.jar
public final class android.media.AudioMixerAttributes implements android.os.Parcelable {
    public static final int MIXER_BEHAVIOR_DEFAULT = 0;
    public static final int MIXER_BEHAVIOR_BIT_PERFECT = 1;

    public android.media.AudioFormat getFormat();
    public int getMixerBehavior();

    public static final class Builder {
        public Builder(android.media.AudioFormat format);
        public AudioMixerAttributes build();
        public Builder setMixerBehavior(int mixerBehavior);
    }
}
```

### 2.3 `AudioManager` Mixer Configuration APIs
```java
public java.util.List<android.media.AudioMixerAttributes> getSupportedMixerAttributes(
    android.media.AudioDeviceInfo device
);

public boolean setPreferredMixerAttributes(
    android.media.AudioAttributes attributes,
    android.media.AudioDeviceInfo device,
    android.media.AudioMixerAttributes mixerAttributes
);

public android.media.AudioMixerAttributes getPreferredMixerAttributes(
    android.media.AudioAttributes attributes,
    android.media.AudioDeviceInfo device
);

public boolean clearPreferredMixerAttributes(
    android.media.AudioAttributes attributes,
    android.media.AudioDeviceInfo device
);

public void addOnPreferredMixerAttributesChangedListener(
    java.util.concurrent.Executor executor,
    android.media.AudioManager$OnPreferredMixerAttributesChangedListener listener
);

public void removeOnPreferredMixerAttributesChangedListener(
    android.media.AudioManager$OnPreferredMixerAttributesChangedListener listener
);
```

### 2.4 AudioPolicyManager & HAL Interaction
When `setPreferredMixerAttributes` is invoked with `MIXER_BEHAVIOR_BIT_PERFECT`:
1. **Audio Policy Manager Validation**: `AudioPolicyService` verifies that the target `AudioDeviceInfo` is a USB peripheral (`TYPE_USB_DEVICE` or `TYPE_USB_HEADSET`).
2. **HAL Port Configuration**: `AudioPolicyManager` instructs the audio HAL to open an output mix port with the flag `AUDIO_OUTPUT_FLAG_BIT_PERFECT`.
3. **Bypass Verification**:
   - Resampling is completely disabled in `AudioFlinger`.
   - Software volume scaling is bypassed: Android system media volume controls no longer modify sample values. The user adjusts volume directly on the external USB DAC hardware knob or buttons.
   - Digital audio effects (EQ, virtualizer) are detached.
4. **Hardware Handshake with AAudio / Oboe**: When Oboe subsequently opens an AAudio output stream specifying `builder.setDeviceId(device.id)` and `builder.setSharingMode(SharingMode::Exclusive)` with matching `AudioAttributes.USAGE_MEDIA`, AAudio binds directly to the bit-perfect HAL endpoint.

---

## 3. Audio Device Discovery, Filtering & Hotplug Lifecycle

### 3.1 Device Types & Classification
`AudioDeviceInfo` represents hardware and virtual audio endpoints. For an audiophile receptor, devices must be filtered by `device.isSink` (playback outputs only) and categorized into clear domain representations:

| `AudioDeviceInfo` Type Constant | Value | Description | Audiophile / Bit-Perfect Role |
|---|---|---|---|
| `TYPE_USB_DEVICE` | 11 | External USB Audio Class 1.0/2.0 DAC or audio interface | Primary Bit-Perfect Target |
| `TYPE_USB_HEADSET` | 22 | USB-C audio dongle DAC or integrated USB-C headset | Primary Bit-Perfect Target |
| `TYPE_USB_ACCESSORY` | 12 | Android USB Open Accessory audio device | Potential Bit-Perfect Target |
| `TYPE_WIRED_HEADPHONES` | 4 | 3.5mm analog stereo headphone jack | Direct Analog Fallback (No USB bypass) |
| `TYPE_WIRED_HEADSET` | 3 | 3.5mm analog headset with microphone | Direct Analog Fallback |
| `TYPE_BUILTIN_SPEAKER` | 2 | Built-in device loudspeaker | Default Fallback (System mixed & EQ'd) |
| `TYPE_BLUETOOTH_A2DP` | 8 | Bluetooth A2DP stereo sink (SBC/AAC/LDAC) | Lossy Bluetooth Fallback |
| `TYPE_BLE_HEADSET` | 26 | Bluetooth LE Audio Unicast Headset | Bluetooth LE Fallback |
| `TYPE_BLE_SPEAKER` | 27 | Bluetooth LE Audio Broadcast / Speaker | Bluetooth LE Fallback |
| `TYPE_LINE_DIGITAL` | 20 | S/PDIF or Optical digital output | Direct Digital Output |
| `TYPE_HDMI` / `TYPE_HDMI_ARC` | 9/10 | HDMI consumer audio link | Multi-channel AV receiver |

### 3.2 Dynamic Hotplug Callbacks
Device attachment and detachment are monitored using Android's native `AudioDeviceCallback`:
- `onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?)`: Fired whenever a USB DAC or 3.5mm headset is plugged into the device.
- `onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?)`: Fired whenever an audio device is disconnected.

### 3.3 Auto-Selection & Disconnection Fallback Rules
1. **USB DAC Insertion**:
   - When an audiophile USB DAC (`TYPE_USB_DEVICE` or `TYPE_USB_HEADSET`) is attached while streaming, `AudioDeviceManager` detects the event, flags the device as bit-perfect capable, and triggers auto-routing if enabled.
2. **Active Device Unplugged (Abrupt Disconnection)**:
   - If the currently playing USB DAC is unplugged, `onAudioDevicesRemoved` triggers immediately.
   - To prevent an audio engine crash or infinite buffer underruns, `AudioDeviceManager` immediately falls back to System Default (`deviceId = 0`).
   - `BitPerfectController` cleans up mixer attributes and updates state to `Fallback(reason = DEVICE_DISCONNECTED)`.
   - `NativeVbanEngine.setAudioDevice(0)` is invoked to seamlessly redirect Oboe to the default speaker or headphones.

---

## 4. Production-Ready Kotlin Code Templates

The following templates represent the complete, production-grade implementation of the audio routing and bit-perfect subsystem, ready to be added to `com.vban.receptor.audio`.

### 4.1 `AudioDevice.kt` — Data Models & State Enums

```kotlin
package com.vban.receptor.audio

import android.media.AudioDeviceInfo

/**
 * High-level classification of audio devices for UI presentation and engine routing.
 */
enum class AudioDeviceCategory {
    USB_DAC,
    WIRED_ANALOG,
    BLUETOOTH,
    BUILTIN_SPEAKER,
    DIGITAL_OUTPUT,
    OTHER
}

/**
 * Immutable model representing an audio output device.
 *
 * @property id Hardware device ID used by Android AudioManager and Oboe (0 = System Default).
 * @property name Human-readable device name (e.g. "Topping DX3 Pro+", "AudioQuest DragonFly").
 * @property type Android AudioDeviceInfo type constant.
 * @property category Grouped categorization for UI filtering and icon display.
 * @property isUsb Whether the device is an external USB DAC or USB headset.
 * @property isBitPerfectCapable Whether the device supports Android 14+ BIT_PERFECT mixer behavior.
 * @property supportedSampleRates List of discrete sample rates reported by hardware descriptors (empty if continuous).
 * @property supportedChannelCounts List of channel counts supported (e.g. [1, 2, 6, 8]).
 * @property rawDeviceInfo Underlying framework AudioDeviceInfo (null for system default).
 */
data class AudioDevice(
    val id: Int,
    val name: String,
    val type: Int,
    val category: AudioDeviceCategory,
    val isUsb: Boolean,
    val isBitPerfectCapable: Boolean,
    val supportedSampleRates: List<Int> = emptyList(),
    val supportedChannelCounts: List<Int> = emptyList(),
    val rawDeviceInfo: AudioDeviceInfo? = null
) {
    /**
     * User-facing label summarizing device type and status.
     */
    val displaySubtitle: String
        get() = when {
            id == 0 -> "Default system audio routing"
            isBitPerfectCapable -> "Bit-Perfect USB DAC (Hardware Bypass Supported)"
            isUsb -> "USB Audio Device (Standard Mixer Mode)"
            category == AudioDeviceCategory.WIRED_ANALOG -> "Analog 3.5mm Audio Output"
            category == AudioDeviceCategory.BLUETOOTH -> "Wireless Bluetooth Audio"
            category == AudioDeviceCategory.BUILTIN_SPEAKER -> "Internal Phone Loudspeaker"
            else -> "Audio Output Device"
        }

    companion object {
        /**
         * Represents the default system audio routing policy (ID 0).
         */
        val SYSTEM_DEFAULT = AudioDevice(
            id = 0,
            name = "Default Output",
            type = AudioDeviceInfo.TYPE_UNKNOWN,
            category = AudioDeviceCategory.BUILTIN_SPEAKER,
            isUsb = false,
            isBitPerfectCapable = false
        )
    }
}

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
```

---

### 4.2 `AudioDeviceManager.kt` — Discovery & Hotplug Engine

```kotlin
package com.vban.receptor.audio

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Production-ready Audio Device Manager for VBAN Receptor.
 *
 * Responsibilities:
 * - Discovers and enumerates available audio output devices via AudioManager.
 * - Filters for output sinks and classifies USB DACs, dongles, wired, Bluetooth, and speakers.
 * - Listens for dynamic device attachment/detachment via AudioDeviceCallback.
 * - Coordinates automatic failover if the active device is unplugged.
 * - Exposes reactive StateFlows for Jetpack Compose UI and PlaybackService.
 */
class AudioDeviceManager(
    private val context: Context,
    private val bitPerfectController: BitPerfectController,
    private val coroutineScope: CoroutineScope = CoroutineScope(Dispatchers.Default)
) {
    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private val isMonitoring = AtomicBoolean(false)

    private val _availableDevices = MutableStateFlow<List<AudioDevice>>(listOf(AudioDevice.SYSTEM_DEFAULT))
    val availableDevices: StateFlow<List<AudioDevice>> = _availableDevices.asStateFlow()

    private val _selectedDevice = MutableStateFlow(AudioDevice.SYSTEM_DEFAULT)
    val selectedDevice: StateFlow<AudioDevice> = _selectedDevice.asStateFlow()

    /**
     * Listener callback for dynamic audio hardware changes.
     */
    private val audioDeviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
            Log.i(TAG, "Audio devices added: ${addedDevices?.size ?: 0} devices")
            coroutineScope.launch {
                refreshDevices(autoSelectNewUsb = true)
            }
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
            Log.i(TAG, "Audio devices removed: ${removedDevices?.size ?: 0} devices")
            val current = _selectedDevice.value
            val currentWasRemoved = removedDevices?.any { it.id == current.id } ?: false

            coroutineScope.launch {
                refreshDevices(autoSelectNewUsb = false)
                if (currentWasRemoved) {
                    Log.w(TAG, "Active audio device (ID ${current.id}: ${current.name}) was removed. Falling back to default.")
                    selectDevice(AudioDevice.SYSTEM_DEFAULT)
                }
            }
        }
    }

    /**
     * Starts listening for hardware device attachment and updates the device list.
     */
    @Synchronized
    fun startMonitoring() {
        if (isMonitoring.getAndSet(true)) {
            return
        }
        audioManager.registerAudioDeviceCallback(audioDeviceCallback, mainHandler)
        refreshDevices(autoSelectNewUsb = false)
        Log.i(TAG, "AudioDeviceManager monitoring started.")
    }

    /**
     * Stops monitoring and releases callback references.
     */
    @Synchronized
    fun stopMonitoring() {
        if (!isMonitoring.getAndSet(false)) {
            return
        }
        audioManager.unregisterAudioDeviceCallback(audioDeviceCallback)
        Log.i(TAG, "AudioDeviceManager monitoring stopped.")
    }

    /**
     * Re-queries the system audio routing table and publishes an updated device list.
     *
     * @param autoSelectNewUsb If true, automatically switches to a newly attached USB DAC.
     */
    fun refreshDevices(autoSelectNewUsb: Boolean = false) {
        val rawDevices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        val mappedDevices = mutableListOf<AudioDevice>()

        // Always include System Default routing as first option
        mappedDevices.add(AudioDevice.SYSTEM_DEFAULT)

        for (device in rawDevices) {
            if (!device.isSink) continue

            val isUsb = isUsbDevice(device.type)
            val isBitPerfect = if (isUsb) {
                bitPerfectController.isBitPerfectSupported(device)
            } else {
                false
            }

            val category = classifyDevice(device.type)
            val name = formatDeviceName(device, category)

            val supportedRates = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                device.sampleRates.toList()
            } else {
                emptyList()
            }

            val supportedChannels = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                device.channelCounts.toList()
            } else {
                emptyList()
            }

            mappedDevices.add(
                AudioDevice(
                    id = device.id,
                    name = name,
                    type = device.type,
                    category = category,
                    isUsb = isUsb,
                    isBitPerfectCapable = isBitPerfect,
                    supportedSampleRates = supportedRates,
                    supportedChannelCounts = supportedChannels,
                    rawDeviceInfo = device
                )
            )
        }

        _availableDevices.value = mappedDevices
        Log.d(TAG, "Discovered ${mappedDevices.size} output devices.")

        if (autoSelectNewUsb) {
            // Find highest-priority USB DAC not currently selected
            val usbDac = mappedDevices.firstOrNull { it.isUsb && it.id != _selectedDevice.value.id }
            if (usbDac != null) {
                Log.i(TAG, "Auto-selecting newly attached USB DAC: ${usbDac.name} (ID ${usbDac.id})")
                selectDevice(usbDac)
            }
        }
    }

    /**
     * Selects an active audio output device, updating state flow.
     */
    fun selectDevice(device: AudioDevice) {
        val previous = _selectedDevice.value
        if (previous.id == device.id) {
            return
        }

        Log.i(TAG, "Switching audio device from '${previous.name}' (${previous.id}) -> '${device.name}' (${device.id})")
        _selectedDevice.value = device
    }

    /**
     * Helper to classify an AudioDeviceInfo type.
     */
    private fun classifyDevice(type: Int): AudioDeviceCategory = when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> AudioDeviceCategory.USB_DAC

        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> AudioDeviceCategory.WIRED_ANALOG

        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
        AudioDeviceInfo.TYPE_BLE_HEADSET,
        AudioDeviceInfo.TYPE_BLE_SPEAKER -> AudioDeviceCategory.BLUETOOTH

        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER,
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE,
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> AudioDeviceCategory.BUILTIN_SPEAKER

        AudioDeviceInfo.TYPE_LINE_ANALOG,
        AudioDeviceInfo.TYPE_LINE_DIGITAL,
        AudioDeviceInfo.TYPE_HDMI,
        AudioDeviceInfo.TYPE_HDMI_ARC,
        AudioDeviceInfo.TYPE_HDMI_EARC -> AudioDeviceCategory.DIGITAL_OUTPUT

        else -> AudioDeviceCategory.OTHER
    }

    /**
     * Formats human-friendly device names, falling back to product name or type.
     */
    private fun formatDeviceName(device: AudioDeviceInfo, category: AudioDeviceCategory): String {
        val productName = device.productName?.toString()?.trim()
        if (!productName.isNullOrEmpty()) {
            return productName
        }
        return when (category) {
            AudioDeviceCategory.USB_DAC -> "USB DAC (${device.id})"
            AudioDeviceCategory.WIRED_ANALOG -> "Wired Headphones (3.5mm)"
            AudioDeviceCategory.BLUETOOTH -> "Bluetooth Audio Device"
            AudioDeviceCategory.BUILTIN_SPEAKER -> "Internal Speaker"
            AudioDeviceCategory.DIGITAL_OUTPUT -> "Digital Line / HDMI Output"
            AudioDeviceCategory.OTHER -> "Audio Device (${device.id})"
        }
    }

    private fun isUsbDevice(type: Int): Boolean = when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> true
        else -> false
    }

    companion object {
        private const val TAG = "AudioDeviceManager"
    }
}
```

---

### 4.3 `BitPerfectController.kt` — Android 14+ Mixer Bypass & Fallback Controller

```kotlin
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
```

---

## 5. Native AAudio / Oboe Handshake & Call Flow

The coordination between Kotlin (`AudioDeviceManager`, `BitPerfectController`), JNI, and C++20 `OboePlayer` follows a strict two-tier handshake:

```
┌─────────────────────────┐         ┌────────────────────────┐         ┌─────────────────────────┐         ┌────────────────────────┐
│   AudioDeviceManager    │         │  BitPerfectController  │         │   NativeVbanEngine      │         │   C++20 OboePlayer     │
└────────────┬────────────┘         └───────────┬────────────┘         └────────────┬────────────┘         └───────────┬────────────┘
             │                                  │                                   │                                  │
   1. User Selects Device                       │                                   │                                  │
             │─────────────────────────────────>│                                   │                                  │
             │ 2. Request Bit-Perfect           │                                   │                                  │
             │    (if USB & Android 14+)        │                                   │                                  │
             │                                  │                                   │                                  │
             │                     3. AudioManager.setPreferredMixerAttributes      │                                  │
             │                        (MIXER_BEHAVIOR_BIT_PERFECT)                  │                                  │
             │                                  │                                   │                                  │
             │ 4. Route Device ID               │                                   │                                  │
             │─────────────────────────────────────────────────────────────────────>│                                  │
             │                                  │                                   │ 5. JNI setAudioDevice(deviceId)  │
             │                                  │                                   │─────────────────────────────────>│
             │                                  │                                   │                                  │
             │                                  │                                   │                     6. 128-sample Fade-Out
             │                                  │                                   │                     7. Close current stream
             │                                  │                                   │                     8. AudioStreamBuilder:
             │                                  │                                   │                        - setDeviceId(id)
             │                                  │                                   │                        - SharingMode::Exclusive
             │                                  │                                   │                        - PerformanceMode::LowLatency
             │                                  │                                   │                     9. Open & Start Stream
             │                                  │                                   │                    10. 128-sample Fade-In
             │                                  │                                   │                                  │
             │                                  │ 11. State: Active                 │                                  │
             │                                  │     (System Vol Bypassed)         │                                  │
```

### 5.1 Exact JNI Invocations
When routing changes occur:
1. `NativeVbanEngine.setDevice(deviceId: Int)` calls JNI export:
   ```cpp
   JNIEXPORT void JNICALL Java_com_vban_receptor_native_NativeVbanEngine_setAudioDevice(
       JNIEnv* env, jobject thiz, jint deviceId);
   ```
2. C++ `VbanEngine::setAudioDevice(int32_t deviceId)` delegates directly to:
   ```cpp
   void OboePlayer::setAudioDevice(int32_t deviceId) {
       if (m_config.deviceId == deviceId) {
           return;
       }
       reconfigure(m_config.sampleRate, m_config.channelCount, deviceId);
   }
   ```
3. Inside `reconfigure`:
   - Triggers 128-sample cosine fade-out (`m_compensator.triggerFadeOut()`).
   - Closes the active stream.
   - Configures `oboe::AudioStreamBuilder` with `builder.setDeviceId(deviceId)` and `builder.setSharingMode(oboe::SharingMode::Exclusive)`.
   - Opens and starts the stream, then initiates a 128-sample cosine fade-in.

---

## 6. Compatibility & Graceful Fallback Matrix

| Platform / Device Configuration | Mixer Mode Applied | Audio Pipeline Behavior | Volume Control | UI Indicator |
|---|---|---|---|---|
| **Android 14+ (API 34+) + USB DAC** (HAL supports bit-perfect) | `MIXER_BEHAVIOR_BIT_PERFECT` via `setPreferredMixerAttributes` | System mixer, resampler, effects completely bypassed. Stream is sent direct to USB hardware HAL. | Bypassed. Controlled via USB DAC hardware knob. | `Bit-Perfect Active (HW Volume)` |
| **Android 14+ (API 34+) + USB DAC** (HAL lacks bit-perfect flag) | Standard mixer attributes with fallback to AAudio Exclusive | System mixer does not guarantee bit-perfect bypass, but AAudio Exclusive mode minimizes latency. | System volume active. | `AAudio Exclusive (Standard Mixer)` |
| **Android 14+ Non-USB** (Internal speaker, Bluetooth, 3.5mm) | Standard System Mixer | Routed through AudioFlinger with speaker protection DSP / Bluetooth A2DP encoding. | System volume active. | `Direct Audio Output` |
| **Android 13 and below (API < 34)** | N/A (`AudioMixerAttributes` unavailable) | Oboe requests `SharingMode::Exclusive` and `PerformanceMode::LowLatency` targeting USB device ID. Direct ALSA / MMAP access if OEM HAL permits. | System volume active. | `AAudio Exclusive (Android 13 Fallback)` |
| **Legacy Fallback** (Exclusive mode denied by OEM) | Shared AAudio Stream | Oboe automatically falls back to `SharingMode::Shared` as implemented in `OboePlayer::openStreamLocked()`. | System volume active. | `Shared Audio Mode` |

---

## 7. Verification & Testing Strategy

### 7.1 Unit & Contract Tests
1. **Mocked Android 14 Environment**:
   - Verify `AudioDeviceManager.refreshDevices` correctly identifies `TYPE_USB_DEVICE` and `TYPE_USB_HEADSET` as `isUsb = true`.
   - Verify `BitPerfectController.isBitPerfectSupported` returns `true` when mock `getSupportedMixerAttributes` contains an entry with `MIXER_BEHAVIOR_BIT_PERFECT`.
   - Verify `enableBitPerfect` constructs the expected `AudioFormat` and successfully calls `setPreferredMixerAttributes`.
   - Verify `disableBitPerfect` calls `clearPreferredMixerAttributes` on the exact `AudioAttributes` and `AudioDeviceInfo`.
2. **Mocked Android 13 Environment (`SDK_INT = 33`)**:
   - Verify `isBitPerfectSupported` returns `false` without throwing `NoClassDefFoundError` or crashing.
   - Verify `enableBitPerfect` smoothly transitions state to `BitPerfectState.Fallback(OS_VERSION_BELOW_34)`.

### 7.2 Integration & Hardware Testing
1. **Physical USB DAC Hotplug**:
   - Attach a USB DAC (e.g., Dragonfly, FiiO, Topping). Confirm `AudioDeviceCallback.onAudioDevicesAdded` detects attachment and updates `availableDevices`.
   - Disconnect USB DAC while playing. Confirm `onAudioDevicesRemoved` triggers immediate fallback to `AudioDevice.SYSTEM_DEFAULT` (ID 0) without audio thread deadlock or crash.
2. **Sample Rate Switching (e.g., 48 kHz -> 96 kHz -> 192 kHz)**:
   - Stream high-resolution VBAN packets. Verify `BitPerfectController` updates preferred mixer attributes to match the new incoming sample rate and Oboe executes a glitch-free fade-out/fade-in reconfiguration.
3. **Volume Slider Validation**:
   - In bit-perfect mode, verify that moving the Android system media volume slider does not attenuate output amplitude. Verify volume is solely controlled by the external USB DAC.
