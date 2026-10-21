package com.vban.receptor

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.core.content.ContextCompat
import com.vban.receptor.audio.AudioDeviceManager
import com.vban.receptor.audio.BitPerfectController
import com.vban.receptor.service.VbanPlaybackService
import com.vban.receptor.ui.DashboardScreen
import com.vban.receptor.ui.theme.VbanReceptorTheme

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
class MainActivity : ComponentActivity() {

    private lateinit var bitPerfectController: BitPerfectController
    private lateinit var audioDeviceManager: AudioDeviceManager

    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted ->
        if (!isGranted) {
            Toast.makeText(
                this,
                "Notification permission required for background audio streaming.",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        bitPerfectController = BitPerfectController(this)
        audioDeviceManager = AudioDeviceManager(this, bitPerfectController)
        audioDeviceManager.startMonitoring()

        requestNotificationPermission()

        setContent {
            VbanReceptorTheme {
                val devices by audioDeviceManager.availableDevices.collectAsState()

                DashboardScreen(
                    onStartService = { ip, port, stream, deviceId ->
                        startVbanService(ip, port, stream, deviceId)
                    },
                    onStopService = {
                        stopVbanService()
                    },
                    availableDevices = devices
                )
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        audioDeviceManager.stopMonitoring()
        bitPerfectController.release()
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(
                    this,
                    Manifest.permission.POST_NOTIFICATIONS
                ) != PackageManager.PERMISSION_GRANTED
            ) {
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
    }

    private fun startVbanService(ip: String, port: Int, stream: String, deviceId: Int) {
        val intent = VbanPlaybackService.createStartIntent(
            context = this,
            listenIp = ip,
            port = port,
            streamName = stream,
            deviceId = deviceId
        )
        ContextCompat.startForegroundService(this, intent)
    }

    private fun stopVbanService() {
        val intent = VbanPlaybackService.createStopIntent(this)
        startService(intent)
    }
}
