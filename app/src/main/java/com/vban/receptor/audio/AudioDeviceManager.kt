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
        @Suppress("DEPRECATION")
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
