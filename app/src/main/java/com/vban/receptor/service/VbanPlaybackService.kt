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
