# Architectural Survey & Production Design Report: Milestone 5
## Android 14+ Foreground Service (`mediaPlayback`) & 3-Lock Power/Network Management

**Author**: `explorer_m5_service`  
**Date**: 2026-10-18  
**Working Directory**: `D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_service`  
**Target Architecture**: Android 14+ (API 34, compileSdk 34, minSdk 26), Kotlin 1.9 / 2.0, AndroidX Core KTX 1.13.1, Coroutines 1.9.0  

---

## 1. Executive Summary & Problem Space

The VBAN Receptor is an audiophile-grade, low-latency audio receptor receiving uncompressed real-time PCM / Float32 audio streams over UDP (Port 6980) and outputting via Oboe / AAudio up to 768 kHz.

Real-time audio over Wi-Fi on Android introduces severe operating system power management hurdles:
1. **Wi-Fi Power-Save Packet Batching (DTIM Sleep)**: Under default Wi-Fi power policies, the mobile radio enters 802.11 power-save mode between Access Point beacon frames (every 100–300 ms). High-rate incoming UDP packets (e.g. 100 to 750 packets/sec) get queued at the AP/chipset and released in bursts, causing 50–200 ms jitter spikes, buffer overflow, and severe audio dropouts.
2. **Kernel Multicast/Broadcast Filtering**: To preserve battery, physical Wi-Fi chipsets on Android drop incoming UDP multicast and broadcast frames at the MAC layer before they reach the Linux kernel network stack. Sockets bound to multicast addresses (e.g., `239.255.0.1`) remain completely silent unless the hardware multicast filter is disabled.
3. **Screen-Off Deep Sleep (Kernel Suspend-to-RAM / Doze Mode)**: When the user turns off the screen or the phone locks, Android attempts to suspend the application processor (AP). While hardware audio DSP offload can sometimes keep playing, the user-space UDP socket worker thread (`vban_receiver`) is immediately halted, stopping all incoming audio.
4. **Android 14 (API 34) Foreground Service Mandates**: Android 14 strictly enforces that any foreground service must explicitly declare its type in the manifest, hold the corresponding granular permission (`FOREGROUND_SERVICE_MEDIA_PLAYBACK`), and provide the exact type flag during `startForeground()`. Failure results in immediate crashes (`SecurityException` or `MissingForegroundServiceTypeException`).
5. **Notification Rate Limiting & System Server Overhead**: Real-time telemetry in `NativeVbanEngine` updates at 30 FPS (every 33 ms). Forwarding updates to `NotificationManager.notify()` at 30 FPS saturates Android's Binder transaction buffer and triggers OS notification throttling.

This report specifies the production architecture, lifecycle, and ready-to-implement Kotlin templates for:
- `PowerNetworkLockManager.kt` (3-Lock: `WIFI_MODE_FULL_LOW_LATENCY`, `PARTIAL_WAKE_LOCK`, `createMulticastLock`)
- `VbanPlaybackService.kt` (Android 14 `mediaPlayback` Foreground Service, low-importance notification channel, 1 Hz throttled telemetry updater, clean binder/intent API)
- `AndroidManifest.xml` (Full permissions and service declaration)

---

## 2. Android 14 (API 34) Foreground Service Specifications

### 2.1 Foreground Service Type Enforcement
Beginning with Android 14 (API level 34, `UPSIDE_DOWN_CAKE`):
- The service declaration in `AndroidManifest.xml` must include:
  ```xml
  android:foregroundServiceType="mediaPlayback"
  ```
- The manifest must declare:
  ```xml
  <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
  <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK" />
  ```
- In Kotlin, when promoting the service to foreground:
  ```kotlin
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
  ```
  *Note*: If `startForeground()` is called without specifying the `mediaPlayback` flag on Android 14, Android will throw `android.app.MissingForegroundServiceTypeException`.

### 2.2 ANR Window & Immediate `startForeground`
When starting a service via `ContextCompat.startForegroundService(context, intent)`:
- The system allocates an execution window of approximately 10 seconds.
- If `startForeground()` is not invoked within this window, Android terminates the host process with:
  `RemoteServiceException: Context.startForegroundService() did not then call Service.startForeground()`
- **Architectural Rule**: In `onStartCommand()`, the notification channel must be verified and `startForeground()` must be invoked **immediately** before doing any socket binding, JNI initialization, or coroutine dispatch.

### 2.3 Clean Service Teardown & Lifecycle
Stopping the foreground service must cleanly reverse all system state:
1. Stop telemetry collection coroutine job.
2. Stop the native C++ engine via `NativeVbanEngine.stop()`.
3. Release all 3 power/network locks via `PowerNetworkLockManager.releaseAll()`.
4. Demote the service from foreground:
   ```kotlin
   if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
       stopForeground(STOP_FOREGROUND_REMOVE)
   } else {
       @Suppress("DEPRECATION")
       stopForeground(true)
   }
   ```
5. Terminate the service via `stopSelf()`.
6. Enforce cleanup idempotency in `onDestroy()`.

---

## 3. 3-Lock Power & Network Management Architecture

Continuous audio streaming requires holding three distinct locks simultaneously:

| Lock Name | System Service & API Call | Mode / Configuration | Required Permission | Hardware Failure Mode Prevented |
|---|---|---|---|---|
| **Low-Latency Wi-Fi Lock** | `WifiManager.createWifiLock()` | `WIFI_MODE_FULL_LOW_LATENCY` (API 29+)<br>Fallback: `WIFI_MODE_FULL_HIGH_PERF` | `android.permission.ACCESS_WIFI_STATE`<br>`android.permission.WAKE_LOCK` | Disables 802.11 power-save DTIM sleep. Prevents AP and Wi-Fi chip packet batching and 50–200 ms latency spikes. |
| **Partial WakeLock** | `PowerManager.newWakeLock()` | `PowerManager.PARTIAL_WAKE_LOCK` | `android.permission.WAKE_LOCK` | Keeps CPU cores executing user-space threads. Prevents kernel suspend-to-RAM when screen is turned off. |
| **Multicast Lock** | `WifiManager.createMulticastLock()` | Non-reference-counted (`setReferenceCounted(false)`) | `android.permission.CHANGE_WIFI_MULTICAST_STATE` | Disables Wi-Fi hardware MAC layer filtering of multicast/broadcast frames. Allows `IP_ADD_MEMBERSHIP` UDP packets to reach the socket. |

### 3.1 Detailed Lock Analysis & Failure Modes

#### 1. `WifiLock` (`WIFI_MODE_FULL_LOW_LATENCY`)
- **Mechanism**: In normal operation, mobile Wi-Fi chipsets sleep between DTIM beacons to conserve battery. Incoming packets are buffered at the router and flushed when the client wakes up. For VBAN, which transmits packets continuously at up to ~750 packets/second, this causes buffer overflows and stuttering.
- **API 29+ Behavior**: `WIFI_MODE_FULL_LOW_LATENCY` disables 802.11 power saving and enables low-latency radio scheduling in the Wi-Fi driver.
- **Reference Counting**: Must be configured with `setReferenceCounted(false)` so that redundant acquire calls do not create leaked reference counts.

#### 2. `WakeLock` (`PARTIAL_WAKE_LOCK`)
- **Mechanism**: Modern Android devices enter Doze Mode / Deep Sleep within minutes of the screen turning off. Deep Sleep halts CPU cores and stops Linux process scheduling.
- **Effect on VBAN**: The native C++ UDP receiver thread (`vban_receiver`) runs on a standard Linux pthread. Without a `PARTIAL_WAKE_LOCK`, the receiver thread freezes immediately when the display turns off, stopping the audio buffer pipeline.

#### 3. `MulticastLock` (`vban_multicast`)
- **Mechanism**: Android Wi-Fi HALs program hardware packet filters in the Wi-Fi NIC firmware to drop all packets where the destination MAC has the multicast bit set (e.g. `01:00:5E:xx:xx:xx`).
- **Effect on VBAN**: If a user configures VBAN to listen on a multicast address (e.g. `239.255.0.1`), the socket will never receive datagrams on a physical Android device unless `MulticastLock.acquire()` is held, even if `IP_ADD_MEMBERSHIP` succeeds in C++.
- **Reference Counting**: Must be set to `setReferenceCounted(false)`.

---

## 4. Notification Subsystem & Telemetry Throttling

### 4.1 Notification Channel Configuration
- **Channel ID**: `"vban_playback_channel"`
- **Channel Name**: `"VBAN Audio Playback"`
- **Importance**: `NotificationManager.IMPORTANCE_LOW`
  - *Rationale*: `IMPORTANCE_LOW` ensures the notification appears persistently in the notification drawer and status bar without producing audible notification chimes, vibration pulses, or intrusive heads-up popups on telemetry updates.
- **Public Lockscreen Visibility**: `Notification.VISIBILITY_PUBLIC` ensures transport controls and stream metadata are visible on the lockscreen.
- **Badging**: `setShowBadge(false)` prevents distracting badge dots on the app icon.

### 4.2 Throttled Telemetry Updates (1 Hz Rate Limiter)
- `NativeVbanEngine.telemetry` emits at 30 Hz (33 ms).
- Pushing 30 notification updates per second to `NotificationManager.notify()` causes:
  1. Excessive Binder IPC overhead between the app process and `system_server`.
  2. Severe OS-level notification rate-limiting / dropping.
  3. Increased CPU and battery consumption.
- **Solution**: Inside `VbanPlaybackService`, a coroutine collects `NativeVbanEngine.telemetry` throttled to **1 Hz (1000 ms)** using Kotlin Flow `sample(1000L)` or a periodic ticker loop.
- The notification text is dynamically updated with:
  `Streaming: 96000 Hz • 24-bit • 2 ch | Loss: 0.0% | Jitter: 0.8ms`

### 4.3 Notification PendingIntent Actions
1. **Content Intent**: Tapping the notification opens `MainActivity` (`FLAG_UPDATE_CURRENT or FLAG_IMMUTABLE`).
2. **Stop Action**: Clicking "Stop" sends an explicit `ACTION_STOP` Intent to `VbanPlaybackService` (`FLAG_UPDATE_CURRENT or FLAG_IMMUTABLE`).

---

## 5. Complete Production Kotlin Code Template: `PowerNetworkLockManager.kt`

Target File: `app/src/main/java/com/vban/receptor/service/PowerNetworkLockManager.kt`

```kotlin
package com.vban.receptor.service

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.PowerManager
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Manages the 3 essential power and network locks required for uninterrupted,
 * audiophile-grade, low-latency VBAN UDP streaming on Android:
 *
 * 1. [WifiManager.WifiLock] with [WifiManager.WIFI_MODE_FULL_LOW_LATENCY]:
 *    Disables 802.11 power-saving (DTIM beacon sleep) to eliminate packet burst jitter.
 * 2. [PowerManager.WakeLock] with [PowerManager.PARTIAL_WAKE_LOCK]:
 *    Prevents the CPU from entering deep sleep when the screen turns off or locks.
 * 3. [WifiManager.MulticastLock]:
 *    Instructs the Wi-Fi hardware MAC filter to allow incoming UDP multicast datagrams.
 *
 * All locks are configured with setReferenceCounted(false) to guarantee idempotent
 * acquisition and release cycles without resource leaks.
 */
class PowerNetworkLockManager(private val context: Context) {

    companion object {
        private const val TAG = "PowerNetworkLockMgr"
        private const val WAKE_LOCK_TAG = "vban_receptor:wake_lock"
        private const val WIFI_LOCK_TAG = "vban_receptor:wifi_lock"
        private const val MULTICAST_LOCK_TAG = "vban_multicast"
    }

    private var wifiLock: WifiManager.WifiLock? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    private val _isAcquired = AtomicBoolean(false)
    val isAcquired: Boolean
        get() = _isAcquired.get()

    /**
     * Acquires all three locks safely. If any individual lock fails due to device-specific
     * restrictions, the error is logged and remaining locks continue to be acquired.
     *
     * @return true if at least the core wake lock was successfully acquired, false otherwise.
     */
    @Synchronized
    fun acquireAll(): Boolean {
        if (_isAcquired.get()) {
            Log.d(TAG, "Locks are already acquired. Skipping.")
            return true
        }

        val appContext = context.applicationContext
        val wifiManager = appContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        val powerManager = appContext.getSystemService(Context.POWER_SERVICE) as? PowerManager

        // 1. Acquire Partial WakeLock (CPU keep-alive)
        if (powerManager != null) {
            try {
                wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK_TAG).apply {
                    setReferenceCounted(false)
                    acquire()
                }
                Log.i(TAG, "WakeLock (PARTIAL_WAKE_LOCK) acquired successfully.")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to acquire WakeLock: ${e.message}", e)
            }
        } else {
            Log.w(TAG, "PowerManager is null; cannot acquire WakeLock.")
        }

        // 2. Acquire Low-Latency Wi-Fi Lock & Multicast Lock
        if (wifiManager != null) {
            // Wi-Fi Lock
            try {
                val wifiMode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                } else {
                    @Suppress("DEPRECATION")
                    WifiManager.WIFI_MODE_FULL_HIGH_PERF
                }
                wifiLock = wifiManager.createWifiLock(wifiMode, WIFI_LOCK_TAG).apply {
                    setReferenceCounted(false)
                    acquire()
                }
                Log.i(TAG, "WifiLock acquired with mode=$wifiMode.")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to acquire WifiLock: ${e.message}", e)
            }

            // Multicast Lock
            try {
                multicastLock = wifiManager.createMulticastLock(MULTICAST_LOCK_TAG).apply {
                    setReferenceCounted(false)
                    acquire()
                }
                Log.i(TAG, "MulticastLock acquired successfully.")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to acquire MulticastLock: ${e.message}", e)
            }
        } else {
            Log.w(TAG, "WifiManager is null; cannot acquire WifiLock or MulticastLock.")
        }

        val acquired = wakeLock?.isHeld == true || wifiLock?.isHeld == true
        _isAcquired.set(acquired)
        return acquired
    }

    /**
     * Releases all held locks safely, catching exceptions per lock to prevent
     * cleanup aborts. Resets internal lock references to null.
     */
    @Synchronized
    fun releaseAll() {
        // Release MulticastLock
        try {
            multicastLock?.let {
                if (it.isHeld) {
                    it.release()
                    Log.i(TAG, "MulticastLock released.")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error releasing MulticastLock: ${e.message}", e)
        } finally {
            multicastLock = null
        }

        // Release WifiLock
        try {
            wifiLock?.let {
                if (it.isHeld) {
                    it.release()
                    Log.i(TAG, "WifiLock released.")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error releasing WifiLock: ${e.message}", e)
        } finally {
            wifiLock = null
        }

        // Release WakeLock
        try {
            wakeLock?.let {
                if (it.isHeld) {
                    it.release()
                    Log.i(TAG, "WakeLock released.")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error releasing WakeLock: ${e.message}", e)
        } finally {
            wakeLock = null
        }

        _isAcquired.set(false)
        Log.i(TAG, "All power and network locks have been safely released.")
    }

    // Diagnostic queries for telemetry and automated testing
    fun isWifiLockHeld(): Boolean = wifiLock?.isHeld == true
    fun isWakeLockHeld(): Boolean = wakeLock?.isHeld == true
    fun isMulticastLockHeld(): Boolean = multicastLock?.isHeld == true
}
```

---

## 6. Complete Production Kotlin Code Template: `VbanPlaybackService.kt`

Target File: `app/src/main/java/com/vban/receptor/service/VbanPlaybackService.kt`

```kotlin
package com.vban.receptor.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import com.vban.receptor.native.NativeTelemetry
import com.vban.receptor.native.NativeVbanEngine
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.sample
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Android 14+ compliant Foreground Service executing continuous VBAN UDP reception
 * and low-latency audio playback under the 'mediaPlayback' service type.
 *
 * Responsibilities:
 * - Immediate startForeground promotion within system ANR budget.
 * - Manages ongoing persistent notification with IMPORTANCE_LOW.
 * - Integrates [PowerNetworkLockManager] for 3-lock power management.
 * - Encapsulates [NativeVbanEngine] lifecycle.
 * - Throttles notification telemetry updates to 1 Hz to prevent IPC rate-limiting.
 * - Exposes Binder interface and Intent Action API for UI and system controls.
 */
class VbanPlaybackService : Service() {

    companion object {
        private const val TAG = "VbanPlaybackService"

        const val CHANNEL_ID = "vban_playback_channel"
        const val CHANNEL_NAME = "VBAN Audio Playback"
        const val NOTIFICATION_ID = 1001

        // Intent Actions
        const val ACTION_START = "com.vban.receptor.service.action.START"
        const val ACTION_STOP = "com.vban.receptor.service.action.STOP"
        const val ACTION_SET_DEVICE = "com.vban.receptor.service.action.SET_DEVICE"

        // Intent Extras
        const val EXTRA_LISTEN_IP = "com.vban.receptor.service.extra.LISTEN_IP"
        const val EXTRA_PORT = "com.vban.receptor.service.extra.PORT"
        const val EXTRA_STREAM_NAME = "com.vban.receptor.service.extra.STREAM_NAME"
        const val EXTRA_DEVICE_ID = "com.vban.receptor.service.extra.DEVICE_ID"

        /**
         * Helper to construct a start intent.
         */
        fun createStartIntent(
            context: Context,
            listenIp: String = "0.0.0.0",
            port: Int = 6980,
            streamName: String = "",
            deviceId: Int = 0
        ): Intent {
            return Intent(context, VbanPlaybackService::class.java).apply {
                action = ACTION_START
                putExtra(EXTRA_LISTEN_IP, listenIp)
                putExtra(EXTRA_PORT, port)
                putExtra(EXTRA_STREAM_NAME, streamName)
                putExtra(EXTRA_DEVICE_ID, deviceId)
            }
        }

        /**
         * Helper to construct a stop intent.
         */
        fun createStopIntent(context: Context): Intent {
            return Intent(context, VbanPlaybackService::class.java).apply {
                action = ACTION_STOP
            }
        }

        /**
         * Helper to construct a device switch intent.
         */
        fun createSetDeviceIntent(context: Context, deviceId: Int): Intent {
            return Intent(context, VbanPlaybackService::class.java).apply {
                action = ACTION_SET_DEVICE
                putExtra(EXTRA_DEVICE_ID, deviceId)
            }
        }
    }

    private val binder = VbanPlaybackBinder()
    private val lockManager by lazy { PowerNetworkLockManager(this) }

    private val serviceJob = SupervisorJob()
    private val serviceScope = CoroutineScope(Dispatchers.Main + serviceJob)
    private var telemetryUpdateJob: Job? = null

    private var currentListenIp: String = "0.0.0.0"
    private var currentPort: Int = 6980
    private var currentStreamName: String = ""
    private var currentDeviceId: Int = 0

    private var isForegroundActive: Boolean = false

    inner class VbanPlaybackBinder : Binder() {
        val service: VbanPlaybackService
            get() = this@VbanPlaybackService

        fun isEngineRunning(): Boolean = NativeVbanEngine.isRunning.value

        fun setAudioDevice(deviceId: Int) {
            this@VbanPlaybackService.setAudioDevice(deviceId)
        }

        fun stopPlayback() {
            this@VbanPlaybackService.stopPlayback()
        }
    }

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "VbanPlaybackService onCreate()")
        createNotificationChannel()
    }

    override fun onBind(intent: Intent?): IBinder {
        Log.i(TAG, "Client bound to VbanPlaybackService.")
        return binder
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val action = intent?.action ?: ACTION_START
        Log.i(TAG, "onStartCommand() received action: $action (startId=$startId)")

        when (action) {
            ACTION_START -> {
                val listenIp = intent?.getStringExtra(EXTRA_LISTEN_IP) ?: "0.0.0.0"
                val port = intent?.getIntExtra(EXTRA_PORT, 6980) ?: 6980
                val streamName = intent?.getStringExtra(EXTRA_STREAM_NAME) ?: ""
                val deviceId = intent?.getIntExtra(EXTRA_DEVICE_ID, 0) ?: 0

                startPlayback(listenIp, port, streamName, deviceId)
            }

            ACTION_STOP -> {
                stopPlayback()
            }

            ACTION_SET_DEVICE -> {
                val deviceId = intent?.getIntExtra(EXTRA_DEVICE_ID, 0) ?: 0
                setAudioDevice(deviceId)
            }

            else -> {
                Log.w(TAG, "Unhandled action: $action")
            }
        }

        return START_NOT_STICKY
    }

    /**
     * Starts foreground playback immediately to satisfy the Android 14 ANR deadline.
     */
    @Synchronized
    private fun startPlayback(listenIp: String, port: Int, streamName: String, deviceId: Int) {
        currentListenIp = listenIp
        currentPort = port
        currentStreamName = streamName
        currentDeviceId = deviceId

        // 1. Promote to Foreground IMMEDIATELY
        promoteToForeground(
            title = "VBAN Receptor",
            content = "Connecting to $currentListenIp:$currentPort...",
            subText = "Starting"
        )

        // 2. Acquire 3-Lock Power & Network Guards
        val locksAcquired = lockManager.acquireAll()
        if (!locksAcquired) {
            Log.w(TAG, "Warning: Not all power/network locks were successfully acquired.")
        }

        // 3. Launch Native Engine
        val started = NativeVbanEngine.start(
            listenIp = currentListenIp,
            port = currentPort,
            streamName = currentStreamName,
            deviceId = currentDeviceId
        )

        if (started) {
            Log.i(TAG, "Native VBAN Engine started successfully.")
            startThrottledNotificationUpdates()
        } else {
            Log.e(TAG, "Failed to start Native VBAN Engine. Reverting service.")
            stopPlayback()
        }
    }

    /**
     * Stops playback, releases all locks, removes foreground notification, and calls stopSelf().
     */
    @Synchronized
    fun stopPlayback() {
        Log.i(TAG, "Stopping VBAN Playback...")
        stopThrottledNotificationUpdates()

        // Stop Native C++ Pipeline
        NativeVbanEngine.stop()

        // Release 3-Locks
        lockManager.releaseAll()

        // Remove Foreground Notification
        if (isForegroundActive) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                stopForeground(STOP_FOREGROUND_REMOVE)
            } else {
                @Suppress("DEPRECATION")
                stopForeground(true)
            }
            isForegroundActive = false
        }

        stopSelf()
        Log.i(TAG, "VbanPlaybackService stopped and demoted.")
    }

    /**
     * Dynamically routes audio to a specified device (e.g. newly plugged USB DAC).
     */
    fun setAudioDevice(deviceId: Int) {
        currentDeviceId = deviceId
        NativeVbanEngine.setDevice(deviceId)
        Log.i(TAG, "Switched active audio device to ID: $deviceId")
    }

    // =========================================================================
    // Notification & Foreground Management
    // =========================================================================

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val notificationManager = getSystemService(NotificationManager::class.java) ?: return
            val channel = NotificationChannel(
                CHANNEL_ID,
                CHANNEL_NAME,
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Displays live VBAN audio streaming status and playback controls."
                setShowBadge(false)
                lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            }
            notificationManager.createNotificationChannel(channel)
            Log.d(TAG, "Notification channel created: $CHANNEL_ID")
        }
    }

    private fun buildNotification(
        title: String,
        content: String,
        subText: String? = null
    ): Notification {
        // PendingIntent to launch app UI when notification is clicked
        val launchIntent = packageManager.getLaunchIntentForPackage(packageName)?.apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }
        val contentPendingIntent = launchIntent?.let {
            PendingIntent.getActivity(
                this,
                0,
                it,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
        }

        // PendingIntent to stop playback via Stop action button
        val stopIntent = Intent(this, VbanPlaybackService::class.java).apply {
            action = ACTION_STOP
        }
        val stopPendingIntent = PendingIntent.getService(
            this,
            1,
            stopIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val stopAction = NotificationCompat.Action.Builder(
            android.R.drawable.ic_media_pause,
            "Stop",
            stopPendingIntent
        ).build()

        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle(title)
            .setContentText(content)
            .apply {
                if (subText != null) setSubText(subText)
            }
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
            .addAction(stopAction)

        if (contentPendingIntent != null) {
            builder.setContentIntent(contentPendingIntent)
        }

        return builder.build()
    }

    private fun promoteToForeground(title: String, content: String, subText: String?) {
        val notification = buildNotification(title, content, subText)
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
        isForegroundActive = true
    }

    /**
     * Throttles telemetry updates to the Android notification drawer to 1 Hz (1000 ms).
     */
    private fun startThrottledNotificationUpdates() {
        telemetryUpdateJob?.cancel()
        telemetryUpdateJob = serviceScope.launch {
            while (isActive && isForegroundActive && NativeVbanEngine.isRunning.value) {
                delay(1000L) // 1 Hz rate limiter
                val telemetry = NativeVbanEngine.telemetry.value
                updateNotification(telemetry)
            }
        }
    }

    private fun stopThrottledNotificationUpdates() {
        telemetryUpdateJob?.cancel()
        telemetryUpdateJob = null
    }

    private fun updateNotification(t: NativeTelemetry) {
        if (!isForegroundActive) return

        val title = if (currentStreamName.isNotBlank()) {
            "VBAN: $currentStreamName"
        } else {
            "VBAN Receptor"
        }

        val content = if (t.sampleRate > 0) {
            val lossFormatted = "%.1f".format(t.packetLossRatePercent)
            val jitterFormatted = "%.1f".format(t.jitterMs)
            "${t.sampleRate} Hz • ${t.bitDepth}-bit • ${t.channelCount}ch | Loss: $lossFormatted% | Jitter: ${jitterFormatted}ms"
        } else {
            "Listening on $currentListenIp:$currentPort (Waiting for stream...)"
        }

        val subText = if (t.sampleRate > 0) {
            "Buf: ${t.bufferOccupancyPercent.toInt()}%"
        } else {
            "Idle"
        }

        val updatedNotification = buildNotification(title, content, subText)
        val notificationManager = getSystemService(NotificationManager::class.java)
        notificationManager?.notify(NOTIFICATION_ID, updatedNotification)
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.i(TAG, "VbanPlaybackService onDestroy()")
        stopThrottledNotificationUpdates()
        serviceScope.cancel()

        if (NativeVbanEngine.isRunning.value) {
            NativeVbanEngine.stop()
        }
        lockManager.releaseAll()
        isForegroundActive = false
    }
}
```

---

## 7. Complete Manifest Specification: `AndroidManifest.xml`

Target File: `app/src/main/AndroidManifest.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">

    <!-- Network & Socket Permissions -->
    <uses-permission android:name="android.permission.INTERNET" />
    <uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
    <uses-permission android:name="android.permission.ACCESS_WIFI_STATE" />
    <uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE" />

    <!-- Power & CPU Keep-Alive -->
    <uses-permission android:name="android.permission.WAKE_LOCK" />

    <!-- Foreground Service Permissions -->
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MEDIA_PLAYBACK" />

    <!-- Android 13+ (API 33) Notification Posting Permission -->
    <uses-permission android:name="android.permission.POST_NOTIFICATIONS" />

    <application
        android:label="VBAN Receptor"
        android:hasCode="true">

        <!-- Android 14 Media Playback Foreground Service -->
        <service
            android:name="com.vban.receptor.service.VbanPlaybackService"
            android:enabled="true"
            android:exported="false"
            android:foregroundServiceType="mediaPlayback" />

    </application>

</manifest>
```

---

## 8. Integration Architecture with Kotlin Engine & UI

```
┌─────────────────────────────────────────────────────────────┐
│                     Jetpack Compose UI                      │
│ - DashboardViewModel binds to VbanPlaybackService           │
│ - Collects NativeVbanEngine.telemetry StateFlow at 30 FPS   │
│ - Sends ACTION_START / ACTION_STOP via ContextCompat        │
└──────────────┬──────────────────────────────▲───────────────┘
               │ Intent / Binder API          │ StateFlow
               ▼                              │
┌─────────────────────────────────────────────┴───────────────┐
│                    VbanPlaybackService                      │
│ - Android 14 foregroundServiceType="mediaPlayback"          │
│ - Low-Importance Notification with Stop Action              │
│ - Throttled 1 Hz Notification Telemetry Poller              │
└──────────────┬──────────────────────────────┬───────────────┘
               │ acquireAll() / releaseAll()  │ start() / stop()
               ▼                              ▼
┌──────────────────────────────┐ ┌────────────────────────────┐
│   PowerNetworkLockManager    │ │      NativeVbanEngine      │
│ - WifiLock (Low-Latency)     │ │ - JNI Bridge to C++20      │
│ - WakeLock (Partial)         │ │ - UDP Port 6980 Receiver   │
│ - MulticastLock (Hardware)   │ │ - Oboe AAudio Real-Time    │
└──────────────────────────────┘ └────────────────────────────┘
```

### Key Integration Contracts:
1. **Starting Service from UI**:
   ```kotlin
   val startIntent = VbanPlaybackService.createStartIntent(
       context = context,
       listenIp = uiState.listenIp,
       port = uiState.port,
       streamName = uiState.streamName,
       deviceId = uiState.selectedDeviceId
   )
   ContextCompat.startForegroundService(context, startIntent)
   ```
2. **Stopping Service from UI or Notification**:
   ```kotlin
   val stopIntent = VbanPlaybackService.createStopIntent(context)
   context.startService(stopIntent)
   ```
3. **Audio Device Switching**:
   When an external USB DAC is plugged in or selected in Compose:
   ```kotlin
   val deviceIntent = VbanPlaybackService.createSetDeviceIntent(context, device.id)
   context.startService(deviceIntent)
   ```

---

## 9. Verification & Test Strategy

### 9.1 Unit Test Coverage
To be implemented in `app/src/test/java/com/vban/receptor/service/`:
1. `PowerNetworkLockManagerTest.kt`:
   - Verify `acquireAll()` sets `isReferenceCounted(false)` on all locks.
   - Verify `acquireAll()` is idempotent (calling multiple times does not leak locks).
   - Verify `releaseAll()` checks `isHeld` before calling `release()`.
   - Verify `releaseAll()` resets all internal references to null.
2. `VbanPlaybackServiceIntentTest.kt`:
   - Verify `createStartIntent`, `createStopIntent`, `createSetDeviceIntent` populate correct actions and extras.
   - Verify notification channel constants and low-importance level.

### 9.2 Android System & Build Verification
1. `assembleDebug` execution:
   `./gradlew assembleDebug` validates syntax, imports, manifest merging, and NDK linkage.
2. Manifest validation:
   Verify merged manifest contains all 8 required permissions and `android:foregroundServiceType="mediaPlayback"`.
3. Screen-off audio endurance test:
   Stream audio, lock the screen, verify via logcat that UDP packets continue arriving and `WifiLock` / `WakeLock` maintain active playback for >10 minutes without underrun.
