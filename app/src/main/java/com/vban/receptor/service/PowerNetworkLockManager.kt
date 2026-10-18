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
