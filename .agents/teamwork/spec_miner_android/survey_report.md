# Android Platform & Audio Specifications Survey Report

**Agent**: `spec_miner_android`  
**Date**: 2026-10-18  
**Scope**: Android NDK Audio Engine (Oboe 1.9.0 / AAudio), Android 14+ Bit-Perfect Mixer Bypass, Background Lifecycle & Locks, and Jetpack Compose UI Telemetry.

---

## 1. Executive Summary & Architecture Overview

The VBAN Receptor on Android requires an audiophile-grade, low-latency playback architecture capable of handling multi-rate streams (from 44.1 kHz up to 768 kHz in Float32 or 24-bit PCM), running persistently with the screen locked, and routing uncompressed, un-resampled audio directly to external USB DACs.

### High-Level System Architecture

```
                    +-------------------------------------------------------------+
                    |                       Android Framework                     |
                    |  +--------------------+         +------------------------+  |
                    |  | Jetpack Compose UI | <-----> |   AudioDeviceManager   |  |
                    |  | - Connection Card  |         | - List AudioDeviceInfo |  |
                    |  | - Stereo VU Meter  |         | - Detect USB DAC       |  |
                    |  | - Jitter Gauge     |         | - Set Bit-Perfect Attr |  |
                    |  +---------^----------+         +-----------+------------+  |
                    |            | StateFlow / Polling            |               |
                    |  +---------v--------------------------------v------------+  |
                    |  | VbanPlaybackService (Foreground Service: mediaPlayback)|  |
                    |  | - WifiLock (WIFI_MODE_FULL_LOW_LATENCY)               |  |
                    |  | - WakeLock (PARTIAL_WAKE_LOCK)                        |  |
                    |  | - MulticastLock (CHANGE_WIFI_MULTICAST_STATE)         |  |
                    +--+-------------------------------------------------------+--+
                                                 | JNI Control & Device Routing
                                                 v
                    +-------------------------------------------------------------+
                    |                      Native C++ Engine                      |
                    |  +-------------------------------------------------------+  |
                    |  | VbanReceiver (UDP Socket, 4MB buffer, Worker Thread)  |  |
                    |  +---------------------------+---------------------------+  |
                    |                              | SPSC Push (Frames)           |
                    |  +---------------------------v---------------------------+  |
                    |  | Lock-Free SPSC Ring Buffer (Power-of-2, Cache-Aligned)|  |
                    |  +---------------------------+---------------------------+  |
                    |                              | SPSC Pull (Real-Time Callback|
                    |  +---------------------------v---------------------------+  |
                    |  | OboeAudioEngine (Oboe 1.9.0 + AAudio Exclusive Mode)  |  |
                    |  | - AudioStreamDataCallback (onAudioReady - Zero Alloc) |  |
                    |  | - AudioStreamErrorCallback (onErrorAfterClose Recover)|  |
                    |  | - Clock Drift Compensator (Hermite/Linear Resampler)  |  |
                    |  | - Telemetry Accumulator (Peak/RMS, Jitter ms, Drops)  |  |
                    +--+-------------------------------------------------------+--+
                                                 | MMAP / Direct HAL
                                                 v
                    +-------------------------------------------------------------+
                    |         External USB DAC (Bit-Perfect) / Audio HAL          |
                    +-------------------------------------------------------------+
```

---

## 2. Category 1: Oboe 1.9.0 C++ Audio Engine & AAudio Backend

### 2.1 Exclusive Mode & Low Latency Configuration

Oboe wraps AAudio (on API 26+) and OpenSL ES (on legacy Android), automatically providing the lowest latency path. For audiophile reception, AAudio is strictly prioritized.

#### Stream Builder Configuration:
```cpp
#include <oboe/Oboe.h>

oboe::AudioStreamBuilder builder;
builder.setDirection(oboe::Direction::Output)
       ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
       ->setSharingMode(oboe::SharingMode::Exclusive)
       ->setAudioApi(oboe::AudioApi::AAudio)
       ->setFormat(oboe::AudioFormat::Float)
       ->setChannelCount(2) // Stereo default; configurable up to 8
       ->setSampleRate(sampleRate) // Target sample rate, e.g., 48000 up to 768000
       ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::None) // Prevent hidden Oboe resampler
       ->setDataCallback(dataCallback)
       ->setErrorCallback(errorCallback);

if (targetDeviceId != 0) {
    builder.setDeviceId(targetDeviceId);
}

std::shared_ptr<oboe::AudioStream> stream;
oboe::Result result = builder.openStream(stream);
```

#### Key API Parameters & Meanings:
- **`PerformanceMode::LowLatency`**: Instructs the system to use the fast mixer or MMAP hardware path, disabling software effects and intermediate buffering.
- **`SharingMode::Exclusive`**: Requests direct access to the audio hardware ALSA/DSP endpoint without passing through the multi-client mixer thread (`AudioFlinger`).
- **`AudioApi::AAudio`**: Explicitly chooses AAudio. If AAudio cannot open the device, Oboe can fall back to OpenSL ES, but for sample rates $> 48\text{ kHz}$ or bit-perfect playback, OpenSL ES is inadequate.
- **`SampleRateConversionQuality::None`**: Ensures Oboe does not inject an internal resampler when the rate does not match; our engine handles clock drift and format synchronization explicitly.

### 2.2 Callback Mechanisms

#### AudioStreamDataCallback:
```cpp
class VbanAudioDataCallback : public oboe::AudioStreamDataCallback {
public:
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream *oboeStream,
        void *audioData,
        int32_t numFrames) override 
    {
        float *outputBuffer = static_cast<float *>(audioData);
        // 1. Pull numFrames from SPSC circular buffer
        // 2. Perform clock-drift micro-interpolation if enabled
        // 3. Calculate Peak & RMS telemetry metrics atomically
        // 4. If buffer underflow: fill remainder with silence (0.0f)
        return oboe::DataCallbackResult::Continue;
    }
};
```

**Real-Time Thread Constraints**:
1. **Zero Allocations**: No `malloc`, `new`, `std::vector::resize`, or object creation.
2. **Zero Blocking Calls**: No `std::mutex::lock` (only lock-free atomics `std::atomic`), no socket reads/writes, no file I/O, no logging (`__android_log_print` should be avoided or replaced with lock-free ring buffer logging).
3. **Execution Deadline**: For a 96-frame burst at 48 kHz, the callback must complete within $2.0\text{ ms}$; at 192 kHz (192 frames), within $1.0\text{ ms}$. Exceeding this budget causes an audio underrun (glitch).

#### AudioStreamErrorCallback & Recovery:
```cpp
class VbanAudioErrorCallback : public oboe::AudioStreamErrorCallback {
public:
    void onErrorAfterClose(oboe::AudioStream *oboeStream, oboe::Result error) override {
        // Invoked on a dedicated background thread spawned by Oboe.
        // The previous stream is already closed.
        if (error == oboe::Result::ErrorDisconnected) {
            // Audio peripheral (e.g. USB DAC or headphones) was unplugged or route changed
            mEngine->handleStreamDisconnected();
        } else {
            // ErrorInvalidState, ErrorTimeout, ErrorInternal, etc.
            mEngine->reopenStream();
        }
    }
};
```

### 2.3 Handling Sample Rates up to 768 kHz with AAudio

VBAN supports up to 768 kHz sample rate (official VBAN sample rate index 20 = 768,000 Hz).
1. **Hardware Capability Query**: Internal phone DACs almost never support 768 kHz (typically capped at 48 kHz, 96 kHz, or 192 kHz). High-end external USB DACs (e.g., ESS Sabre ES9038PRO, AKM AK4499EX, Cirrus Logic CS43198) support 384 kHz and 768 kHz PCM natively over USB Audio Class 2.0 (UAC2).
2. **Negotiated vs. Requested Rate**:
   - Calling `builder.setSampleRate(768000)` on an unsupported internal device will either return `AAUDIO_ERROR_INVALID_RATE` in exclusive mode or fall back to system-resampled shared mode.
   - After `openStream()`, inspect `stream->getSampleRate()`.
   - If `actualRate != requestedRate`, the VBAN engine must dynamically engage its high-quality Hermite or linear polyphase interpolator to resample from the incoming VBAN rate to the stream's native hardware rate.
3. **Buffer Sizing for Ultra-High Sample Rates**:
   - At 768 kHz stereo Float32, 1 second of audio requires $768,000 \times 2 \times 4\text{ bytes} \approx 6.14\text{ MB/s}$.
   - Burst sizes are larger (e.g., 512–2048 frames per callback = $4\text{ KB} - 16\text{ KB}$ per burst).
   - The SPSC ring buffer must allocate at least $32,768$ to $65,536$ frames ($\approx 256\text{ KB} - 512\text{ KB}$) to absorb Wi-Fi network jitter without overflow or underrun.

---

## 3. Category 2: Android 14+ (API 34) Bit-Perfect Mixer Bypass for USB DACs

### 3.1 Framework Architecture & Discovery

In Android versions prior to API 34, Android’s audio server (`AudioFlinger`) forced all PCM streams through a software mixer thread, resampling audio to 48 kHz (or 44.1 kHz), applying volume scaling and DSP limiter effects.

Android 14 (API 34 / `UPSIDE_DOWN_CAKE`) introduced native bit-perfect output for USB audio peripherals via `AudioMixerAttributes`.

#### Decompiled Class Signatures (`android.media.AudioMixerAttributes`):
```java
package android.media;

public final class AudioMixerAttributes implements Parcelable {
    public static final int MIXER_BEHAVIOR_DEFAULT = 0;
    public static final int MIXER_BEHAVIOR_BIT_PERFECT = 1;

    public AudioFormat getFormat();
    public int getMixerBehavior();

    public static final class Builder {
        public Builder(AudioFormat format);
        public Builder setMixerBehavior(int mixerBehavior);
        public AudioMixerAttributes build();
    }
}
```

#### AudioManager Mixer API Signatures:
```java
// Query supported configurations for a connected device
public List<AudioMixerAttributes> getSupportedMixerAttributes(AudioDeviceInfo device);

// Set bit-perfect or custom mixer preference
public boolean setPreferredMixerAttributes(
    AudioAttributes attributes,
    AudioDeviceInfo device,
    AudioMixerAttributes mixerAttributes
);

// Retrieve active mixer attributes
public AudioMixerAttributes getPreferredMixerAttributes(
    AudioAttributes attributes,
    AudioDeviceInfo device
);

// Restore default mixer behavior
public boolean clearPreferredMixerAttributes(
    AudioAttributes attributes,
    AudioDeviceInfo device
);

// Listener for mixer changes
public void addOnPreferredMixerAttributesChangedListener(
    Executor executor,
    AudioManager.OnPreferredMixerAttributesChangedListener listener
);
```

### 3.2 USB DAC Bit-Perfect Detection Protocol

To detect whether a connected USB device supports bit-perfect playback:
```kotlin
fun checkBitPerfectSupport(context: Context, device: AudioDeviceInfo): Boolean {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
        return false // Requires Android 14+
    }
    
    // Bit-perfect is currently defined for USB devices / headsets
    if (device.type != AudioDeviceInfo.TYPE_USB_DEVICE && 
        device.type != AudioDeviceInfo.TYPE_USB_HEADSET &&
        device.type != AudioDeviceInfo.TYPE_USB_ACCESSORY) {
        return false
    }

    val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    val supportedMixers: List<AudioMixerAttributes> = audioManager.getSupportedMixerAttributes(device)

    return supportedMixers.any { it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT }
}
```

### 3.3 Dynamic Routing & Bit-Perfect Activation

When a VBAN stream with a specific sample rate (e.g. 96 kHz, 24-bit/32-bit) begins playing:
```kotlin
fun enableBitPerfectForStream(
    context: Context,
    device: AudioDeviceInfo,
    sampleRate: Int,
    channelCount: Int
): Boolean {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) return false
    val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    val channelMask = if (channelCount == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
    val mediaAudioAttributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()

    val supported = audioManager.getSupportedMixerAttributes(device)
    
    // Find matching bit-perfect attribute for the stream's sample rate
    val targetMixer = supported.firstOrNull { mixer ->
        mixer.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT &&
        mixer.format.sampleRate == sampleRate
    } ?: supported.firstOrNull { 
        it.mixerBehavior == AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT 
    }

    if (targetMixer != null) {
        return audioManager.setPreferredMixerAttributes(mediaAudioAttributes, device, targetMixer)
    }
    return false
}
```

### 3.4 Interaction with Native AAudio / Oboe Streams

1. **Policy Setup in Kotlin/Java**: The application sets `setPreferredMixerAttributes(mediaAttributes, usbDevice, bitPerfectMixer)` via `AudioManager`.
2. **Audio Policy Manager Action**: The Android AudioPolicyManager updates the active HAL mix port for the USB peripheral, configuring the hardware endpoint to the specified sample rate and format and setting `AUDIO_OUTPUT_FLAG_BIT_PERFECT`.
3. **Native Stream Connection**: The native Oboe engine opens the stream passing `builder.setDeviceId(usbDevice.id)` and `builder.setSharingMode(SharingMode::Exclusive)`.
4. **Volume Control Behavior**: In bit-perfect mode, Android disables software volume attenuation. Volume can only be controlled via the physical knob/buttons on the external USB DAC. The UI must indicate "Bit-Perfect Active: System volume bypassed".
5. **Teardown**: Upon stream stop, `audioManager.clearPreferredMixerAttributes(...)` must be called to restore normal mixer behavior for system notifications.

### 3.5 Backward Compatibility & Fallback (Android 13 and below)

- On Android 13 (`TIRAMISU`, API 33) and lower, `AudioMixerAttributes` is unavailable.
- **Fallback**:
  1. Open Oboe with `SharingMode::Exclusive` and `PerformanceMode::LowLatency` targeting `usbDevice.id`.
  2. AAudio MMAP / Direct mode bypasses the standard mixer loop on supported devices, directly writing to the kernel ALSA buffer.
  3. If exclusive mode fails or the device does not support the stream rate, fall back to `SharingMode::Shared` and engage the native resampler.
  4. UI surfaces: *"Direct AAudio Mode (Android 13 Fallback - System volume active)"*.

---

## 4. Category 3: Android Foreground Service & Power/Network Locks

### 4.1 Manifest Declarations

In `AndroidManifest.xml`:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.vban.receptor">

    <!-- Network Permissions -->
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
    <uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
    <uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE" />

    <!-- Power Management -->
    <uses-permission android:name="android.permission.WAKE_LOCK" />

    <!-- Foreground Service Permissions -->
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK" />

    <!-- Notification Permission (Android 13+ / API 33) -->
    <uses-permission android:name="android.permission.POST_NOTIFICATIONS" />

    <application
        android:allowBackup="false"
        android:icon="@mipmap/ic_launcher"
        android:label="@string/app_name"
        android:theme="@style/Theme.VbanReceptor">

        <service
            android:name=".service.VbanPlaybackService"
            android:enabled="true"
            android:exported="false"
            android:foregroundServiceType="mediaPlayback" />

        <activity
            android:name=".ui.MainActivity"
            android:exported="true"
            android:launchMode="singleTop">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
```

### 4.2 Foreground Service Implementation Details

#### Strict Android 14 Requirements:
1. `android:foregroundServiceType="mediaPlayback"` in `<service>`.
2. `<uses-permission android:name="android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK" />`.
3. Explicit service type passed to `startForeground()`:
```kotlin
class VbanPlaybackService : Service() {

    companion object {
        const val NOTIFICATION_ID = 1001
        const val CHANNEL_ID = "vban_playback_channel"
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        createNotificationChannel()
        val notification = buildNotification("VBAN Receptor Running", "Listening on port 6980...")
        
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }
        
        acquireLocks()
        startAudioEngine()
        return START_STICKY
    }
}
```

### 4.3 Power & Network Locks Specification

High-throughput, low-latency UDP audio streams fail severely if the OS drops the CPU frequency, puts the Wi-Fi chip into power-save sleep, or discards multicast/broadcast frames.

| Lock Type | API Class & Constant | Required Permission | Acquisition Method & Flags | Rationale / Failure Mode Avoided |
|---|---|---|---|---|
| **Low-Latency Wi-Fi Lock** | `WifiManager.WifiLock`<br>`WIFI_MODE_FULL_LOW_LATENCY` (API 29+) | `ACCESS_WIFI_STATE`<br>`WAKE_LOCK` | `wifiManager.createWifiLock(WIFI_MODE_FULL_LOW_LATENCY, "VBAN:WifiLock").apply { setReferenceCounted(false); acquire() }` | Disables Wi-Fi power save beacon sleep (802.11 DTIM sleep). Prevents 50–150ms packet burst jitter. |
| **Partial WakeLock** | `PowerManager.WakeLock`<br>`PARTIAL_WAKE_LOCK` | `android.permission.WAKE_LOCK` | `powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VBAN:WakeLock").apply { setReferenceCounted(false); acquire() }` | Keeps CPU cores active and prevents deep system sleep when screen turns off. |
| **Multicast Lock** | `WifiManager.MulticastLock` | `android.permission.CHANGE_WIFI_MULTICAST_STATE` | `wifiManager.createMulticastLock("VBAN:MulticastLock").apply { setReferenceCounted(false); acquire() }` | Without this lock, Wi-Fi hardware drops all UDP multicast & broadcast packets at the MAC filter level. |

#### Safe Lock Management Pattern:
```kotlin
class VbanLockManager(private val context: Context) {
    private var wifiLock: WifiManager.WifiLock? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    fun acquireLocks() {
        val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val powerManager = context.applicationContext.getSystemService(Context.POWER_SERVICE) as PowerManager

        // 1. WifiLock (Low Latency)
        if (wifiLock == null) {
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                @Suppress("DEPRECATION")
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifiLock = wifiManager.createWifiLock(mode, "VBAN:WifiLock").apply {
                setReferenceCounted(false)
                acquire()
            }
        }

        // 2. Partial WakeLock
        if (wakeLock == null) {
            wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "VBAN:WakeLock").apply {
                setReferenceCounted(false)
                acquire()
            }
        }

        // 3. MulticastLock
        if (multicastLock == null) {
            multicastLock = wifiManager.createMulticastLock("VBAN:MulticastLock").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    fun releaseLocks() {
        wifiLock?.takeIf { it.isHeld }?.release()
        wifiLock = null

        wakeLock?.takeIf { it.isHeld }?.release()
        wakeLock = null

        multicastLock?.takeIf { it.isHeld }?.release()
        multicastLock = null
    }
}
```

---

## 5. Category 4: Jetpack Compose UI & Real-Time Telemetry Pipeline

### 5.1 Reactive State Architecture

The UI observes a single reactive state model powered by Kotlin `StateFlow`, collected in Compose via `collectAsStateWithLifecycle()`.

```kotlin
data class VbanUiState(
    // Connection Controls
    val isRunning: Boolean = false,
    val bindAddress: String = "0.0.0.0",
    val port: Int = 6980,
    val streamName: String = "Stream1",
    
    // Audio Device
    val selectedDevice: AudioDeviceInfo? = null,
    val availableDevices: List<AudioDeviceInfo> = emptyList(),
    val isBitPerfectActive: Boolean = false,
    val isBitPerfectSupported: Boolean = false,

    // Real-Time Telemetry
    val leftRmsDb: Float = -60f,
    val rightRmsDb: Float = -60f,
    val leftPeakDb: Float = -60f,
    val rightPeakDb: Float = -60f,
    val bufferFillPercent: Float = 0f,
    val bufferFillMs: Float = 0f,
    val detectedSampleRate: Int = 0,
    val detectedBitDepth: String = "None",
    val detectedChannels: Int = 0,
    val packetsReceived: Long = 0L,
    val packetsLost: Long = 0L,
    val clockDriftPpm: Float = 0f
)
```

### 5.2 Real-Time Stereo VU Meter Implementation

To achieve 60 FPS silky-smooth rendering without garbage collection stalls:
- Compute Peak and RMS in native C++ on each audio chunk.
- Expose atomic values read by a coroutine running at ~40–60 Hz.
- Render on Jetpack Compose `Canvas` using primitive drawing commands.

#### Decibel Conversion & Peak Decay Formula:
$$\text{RMS}_{\text{dB}} = 20 \log_{10}\left(\max\left(\text{RMS}, 10^{-3}\right)\right)$$
- Full scale: $0\text{ dBFS}$ (RMS $= 1.0$)
- Floor: $-60\text{ dBFS}$ (RMS $\approx 0.001$)
- Normalization to $[0.0, 1.0]$: $\text{Level} = \frac{\text{RMS}_{\text{dB}} - (-60)}{60} = \frac{\text{RMS}_{\text{dB}} + 60}{60}$
- Peak Hold Decay: $\text{Peak}_{\text{held}}(t) = \max\left(\text{Peak}_{\text{current}}, \text{Peak}_{\text{held}}(t - \Delta t) - (\text{decayRate} \times \Delta t)\right)$ where $\text{decayRate} = 20\text{ dB/s}$.

#### Compose VU Meter Component Blueprint:
```kotlin
@Composable
fun StereoVuMeter(
    leftRmsDb: Float,
    rightRmsDb: Float,
    leftPeakDb: Float,
    rightPeakDb: Float,
    modifier: Modifier = Modifier
) {
    Column(modifier = modifier.fillMaxWidth().padding(16.dp)) {
        VuMeterChannel("L", leftRmsDb, leftPeakDb)
        Spacer(modifier = Modifier.height(8.dp))
        VuMeterChannel("R", rightRmsDb, rightPeakDb)
    }
}

@Composable
fun VuMeterChannel(label: String, rmsDb: Float, peakDb: Float) {
    val normRms = ((rmsDb + 60f) / 60f).coerceIn(0f, 1f)
    val normPeak = ((peakDb + 60f) / 60f).coerceIn(0f, 1f)

    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(text = label, modifier = Modifier.width(20.dp), fontWeight = FontWeight.Bold)
        Canvas(modifier = Modifier.fillMaxWidth().height(18.dp)) {
            val width = size.width
            val height = size.height
            
            // Background bar
            drawRoundRect(
                color = Color.DarkGray,
                cornerRadius = CornerRadius(4.dp.toPx(), 4.dp.toPx())
            )
            
            // Segmented / Gradient Fill (Green -> Yellow -> Red)
            val fillBrush = Brush.horizontalGradient(
                0.0f to Color(0xFF00E676),  // Green
                0.7f to Color(0xFFFFD600),  // Yellow
                0.9f to Color(0xFFFF1744)   // Red
            )
            
            drawRoundRect(
                brush = fillBrush,
                size = Size(width * normRms, height),
                cornerRadius = CornerRadius(4.dp.toPx(), 4.dp.toPx())
            )

            // Peak indicator line
            if (normPeak > 0f) {
                drawLine(
                    color = Color.White,
                    start = Offset(width * normPeak, 0f),
                    end = Offset(width * normPeak, height),
                    strokeWidth = 2.dp.toPx()
                )
            }
        }
    }
}
```

### 5.3 Audio Device Picker & Hotplug Callback

Reactively tracks USB DAC attachment and route changes:
```kotlin
val audioDeviceCallback = object : AudioDeviceCallback() {
    override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) {
        viewModel.refreshAudioDevices()
    }

    override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) {
        viewModel.refreshAudioDevices()
    }
}

// In ViewModel or Service:
audioManager.registerAudioDeviceCallback(audioDeviceCallback, Handler(Looper.getMainLooper()))
```

### 5.4 Jitter & Buffer Occupancy Gauge

A visual gauge showing the lock-free SPSC buffer fill percentage and latency in milliseconds:
- Optimal Target: $50\%$ fill ($\approx 10\text{–}20\text{ ms}$).
- Visual Color Coding:
  - **Green**: $30\% \le \text{fill} \le 70\%$ (Stable)
  - **Yellow**: $10\% \le \text{fill} < 30\%$ or $70\% < \text{fill} \le 85\%$ (Jitter Drift)
  - **Red**: $<10\%$ (Underrun Hazard) or $>85\%$ (Overflow Hazard)

---

## 6. Category 5: Gradle & CMake Build Configuration

### 6.1 Gradle Prefab Setup

In `app/build.gradle.kts`:
```kotlin
android {
    namespace = "com.vban.receptor"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.vban.receptor"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0"

        externalNativeBuild {
            cmake {
                cppFlags("-std=c++20", "-O3", "-fexceptions", "-frtti")
                arguments("-DANDROID_STL=c++_shared")
            }
        }
        
        ndk {
            abiFilters("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

    buildFeatures {
        compose = true
        prefab = true // Crucial for Oboe AAR linking
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.8"
    }
}

dependencies {
    implementation("com.google.oboe:oboe:1.9.0")
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.7.0")
    implementation("androidx.activity:activity-compose:1.8.2")
    implementation(platform("androidx.compose:compose-bom:2024.02.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
}
```

### 6.2 CMake Configuration (`CMakeLists.txt`)

```cmake
cmake_minimum_required(VERSION 3.22.1)
project(vban_receptor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Prefab-provided Oboe
find_package(oboe REQUIRED CONFIG)

add_library(vban_engine SHARED
    src/main/cpp/vban_engine_jni.cpp
    src/main/cpp/vban_protocol.cpp
    src/main/cpp/audio_engine.cpp
    src/main/cpp/spsc_ring_buffer.cpp
    src/main/cpp/resampler.cpp
)

target_include_directories(vban_engine PRIVATE
    src/main/cpp/include
)

target_link_libraries(vban_engine
    PRIVATE
    oboe::oboe
    aaudio
    android
    log
)
```

---

## 7. Features Discovered Table

| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | Oboe Audio Engine | Low Latency Stream | Creates AAudio exclusive mode low-latency stream | Sample rate, channels, format, performance mode | `oboe::AudioStream` pointer | Returns error result code (e.g. `ErrorUnavailable`) | NDK AAudio.h & Oboe API |
| 2 | Oboe Audio Engine | Real-Time Data Callback | Pulls frames directly into output audio buffer | Output buffer pointer, frame count | `oboe::DataCallbackResult` | Underrun if callback takes too long; zero allocation enforced | Oboe AudioStreamDataCallback |
| 3 | Oboe Audio Engine | Error Recovery Callback | Automatic stream restart on disconnect or route change | `oboeStream`, `oboe::Result` | Rebuilt stream | Re-opening failures guarded by backoff | Oboe AudioStreamErrorCallback |
| 4 | Oboe Audio Engine | Ultra-High Sample Rates (up to 768kHz) | Native playback of high-res VBAN audio streams | 44.1 kHz to 768.0 kHz sample rate | Hardware-matched audio clock | Capped or rejected by HAL if unsupported; adaptive resampler fallback | NDK AAudio & UAC2 specs |
| 5 | Android 14 Bit-Perfect | Supported Mixer Discovery | Queries connected USB device for bit-perfect profiles | `AudioDeviceInfo` | `List<AudioMixerAttributes>` | Empty list if unsupported or API < 34 | `AudioManager.getSupportedMixerAttributes` |
| 6 | Android 14 Bit-Perfect | Preferred Mixer Selection | Bypasses system mixer, volume scaling, and DSP | `AudioAttributes`, `AudioDeviceInfo`, `AudioMixerAttributes` | `Boolean` (success/failure) | Returns `false` if permission missing or device busy | `AudioManager.setPreferredMixerAttributes` |
| 7 | Android 14 Bit-Perfect | Mixer Reset / Teardown | Restores standard shared mixer upon disconnect | `AudioAttributes`, `AudioDeviceInfo` | `Boolean` (success/failure) | Safe no-op if device already removed | `AudioManager.clearPreferredMixerAttributes` |
| 8 | Foreground Service | Media Playback Typing | Mandatory foreground service type for persistent playback | `FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK` | Running Service | Crashes with `MissingForegroundServiceTypeException` if omitted on Android 14 | Android 14 ServiceInfo API |
| 9 | Foreground Service | Media Playback Permission | Specific manifest permission required in Android 14+ | Manifest `<uses-permission>` | System permission granted | `SecurityException` thrown at runtime if absent | Android 14 Manifest Specs |
| 10| System Lifecycle | Notification Permission | Runtime permission required to show playback notification | User prompt / grant | Granted/Denied | Notification hidden if denied; service still runs | Android 13 POST_NOTIFICATIONS |
| 11| Power & Network | Low-Latency Wi-Fi Lock | Disables Wi-Fi power save to prevent packet jitter | `WIFI_MODE_FULL_LOW_LATENCY` | Active lock | Falls back to `HIGH_PERF` on API < 29 | `WifiManager.createWifiLock` |
| 12| Power & Network | Partial CPU WakeLock | Keeps CPU running during screen off | `PARTIAL_WAKE_LOCK` | Active lock | PowerManager handles reference counting | `PowerManager.newWakeLock` |
| 13| Power & Network | Multicast / Broadcast Lock | Enables reception of UDP multicast & broadcast packets | Tag string | Active lock | Sockets drop all multicast/broadcast packets if lock not held | `WifiManager.createMulticastLock` |
| 14| Compose UI | Stereo Peak & RMS VU Meter | Real-time audio level rendering with dB scale & peak hold | Left/Right dB levels | Rendered Canvas meters | Clamped between -60 dBFS and 0 dBFS | Jetpack Compose Canvas |
| 15| Compose UI | Jitter Buffer Gauge | Displays buffer fill level in % and latency in ms | Buffer occupancy & sample rate | Visual health bar (green/yellow/red) | Visual warning on underrun / overflow | Jetpack Compose State |
| 16| Compose UI | Audio Device Selection | Reactive picker for USB DAC, headphones, internal speaker | `AudioDeviceInfo` list | Selected device ID passed to Oboe | Dynamic updates on device connect/disconnect | `AudioDeviceCallback` |
| 17| Native Bridge | Lock-Free Telemetry Polling | Transfers stats from native thread to Kotlin without blocking | C++ atomic struct | Kotlin data object | Non-blocking acquire-release atomic load | JNI & std::atomic |

---

## 8. Edge Cases Table

| # | Feature | Input / Condition | Observed / Documented Behavior | Mitigation / Handling Strategy |
|---|---------|-------------------|--------------------------------|--------------------------------|
| 1 | Oboe Exclusive Mode | Another application already holds the device in exclusive mode | `builder.openStream()` returns `oboe::Result::ErrorUnavailable` | Catch error and retry with `SharingMode::Shared` or notify user in UI. |
| 2 | Sample Rate Negotiation | Stream requests 768 kHz, but USB DAC or phone HAL supports max 192 kHz | AAudio returns `AAUDIO_ERROR_INVALID_RATE` in exclusive mode | Check supported rates via `AudioDeviceInfo.getSampleRates()`; if rate exceeds hardware, dynamically configure native resampler. |
| 3 | USB DAC Disconnection | External USB DAC unplugged mid-playback | Audio hardware interrupts; Oboe fires `onErrorAfterClose` with `ErrorDisconnected` | In `onErrorAfterClose`, signal main thread to switch to default audio device or pause stream cleanly. |
| 4 | Screen Off / Doze Mode | User locks phone while streaming VBAN over Wi-Fi | Without locks, Wi-Fi enters 802.11 powersave sleep; packets arrive in 100ms bursts causing buffer underrun | Hold `WifiLock(WIFI_MODE_FULL_LOW_LATENCY)` and `WakeLock(PARTIAL_WAKE_LOCK)`. |
| 5 | UDP Broadcast Reception | Sender uses `255.255.255.255` instead of unicast | Android Wi-Fi chipset drops broadcast packets by default when screen turns off | Hold `WifiManager.MulticastLock` (required for broadcasts on many chipsets). |
| 6 | Android 14 Bit-Perfect | DAC supports bit-perfect, but vendor HAL omitted `AUDIO_OUTPUT_FLAG_BIT_PERFECT` | `getSupportedMixerAttributes` returns empty list or only `MIXER_BEHAVIOR_DEFAULT` | Gracefully fall back to standard AAudio exclusive mode with appropriate UI notification. |
| 7 | Volume Control in Bit-Perfect | User presses phone hardware volume rocker | System volume has no effect because Android software mixer is bypassed | UI should display badge "Hardware Volume Controlled on USB DAC". |
| 8 | Sample Rate Switch | Sender changes VBAN sample rate (e.g. from 44.1 kHz to 96 kHz) on the fly | Next VBAN packet contains new sample rate index in byte 4 | Detect rate change in packet parser; re-evaluate bit-perfect mixer attributes and restart Oboe stream if necessary. |
| 9 | Oboe Buffer Underrun | High Wi-Fi packet jitter empties SPSC ring buffer | `onAudioReady` finds insufficient frames | Fill output buffer with silence (`0.0f`) to prevent loud static bursts; increment underflow telemetry counter. |
| 10| JNI Telemetry Overhead | 60 FPS UI polling calls JNI allocating Java objects | High GC pressure causing Compose frame drops | Use a reusable primitive array (`floatArrayOf`) or direct ByteBuffer shared between C++ and Kotlin. |
