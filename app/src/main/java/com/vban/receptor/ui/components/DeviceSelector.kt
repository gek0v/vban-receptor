package com.vban.receptor.ui.components

import android.media.AudioDeviceInfo
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.VolumeUp
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Headphones
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.audio.AudioDevice

private val GoldBadgeGradient = Brush.horizontalGradient(
    listOf(Color(0xFFFFD700), Color(0xFFFF9100))
)
private val CyanBadgeGradient = Brush.horizontalGradient(
    listOf(Color(0xFF00E5FF), Color(0xFF0091EA))
)

/**
 * Audio Device Selector card with Android 14+ Bit-Perfect verification badge.
 */
@Composable
fun DeviceSelector(
    devices: List<AudioDevice>,
    selectedDeviceId: Int,
    onDeviceSelected: (AudioDevice) -> Unit,
    modifier: Modifier = Modifier
) {
    var expanded by remember { mutableStateOf(false) }
    val currentDevice = devices.find { it.id == selectedDeviceId }
        ?: devices.firstOrNull()
        ?: AudioDevice.SYSTEM_DEFAULT

    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "AUDIO OUTPUT ROUTING",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(10.dp))

            // Clickable Dropdown Trigger Box
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .border(1.dp, MaterialTheme.colorScheme.outline, RoundedCornerShape(8.dp))
                    .background(Color(0xFF0E1624), RoundedCornerShape(8.dp))
                    .clickable { expanded = true }
                    .padding(horizontal = 14.dp, vertical = 12.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(
                            imageVector = getDeviceIcon(currentDevice.type),
                            contentDescription = "Device Type",
                            tint = if (currentDevice.isUsb) Color(0xFF00E5FF) else MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.size(24.dp)
                        )

                        Spacer(modifier = Modifier.width(12.dp))

                        Column {
                            Text(
                                text = currentDevice.name,
                                style = MaterialTheme.typography.bodyLarge,
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.onSurface
                            )

                            Text(
                                text = getDeviceTypeDescription(currentDevice.type),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }

                    Icon(
                        imageVector = Icons.Default.ArrowDropDown,
                        contentDescription = "Open Device Dropdown",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }

                // Dropdown Options
                DropdownMenu(
                    expanded = expanded,
                    onDismissRequest = { expanded = false },
                    modifier = Modifier.background(Color(0xFF131D2D))
                ) {
                    devices.forEach { device ->
                        DropdownMenuItem(
                            text = {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Icon(
                                        imageVector = getDeviceIcon(device.type),
                                        contentDescription = null,
                                        tint = if (device.isUsb) Color(0xFF00E5FF) else Color.Gray,
                                        modifier = Modifier.size(20.dp)
                                    )
                                    Spacer(modifier = Modifier.width(10.dp))
                                    Column {
                                        Text(device.name, fontWeight = FontWeight.Medium)
                                        if (device.isBitPerfectCapable) {
                                            Text(
                                                "Bit-Perfect Direct Supported",
                                                style = MaterialTheme.typography.labelSmall,
                                                color = Color(0xFFFFD700)
                                            )
                                        }
                                    }
                                }
                            },
                            onClick = {
                                onDeviceSelected(device)
                                expanded = false
                            }
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(10.dp))

            // Audiophile Bit-Perfect Status Badge
            if (currentDevice.isBitPerfectCapable) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(GoldBadgeGradient, RoundedCornerShape(6.dp))
                        .padding(horizontal = 10.dp, vertical = 6.dp)
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.Center,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(
                            text = "★ BIT-PERFECT DIRECT (Android 14 Mixer Bypass Active)",
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black
                        )
                    }
                }
            } else if (currentDevice.isUsb) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(CyanBadgeGradient, RoundedCornerShape(6.dp))
                        .padding(horizontal = 10.dp, vertical = 6.dp)
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.Center,
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(
                            text = "AAUDIO EXCLUSIVE (Low Latency Direct HAL)",
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = FontWeight.Bold,
                            color = Color.Black
                        )
                    }
                }
            } else {
                Text(
                    text = "Standard Shared Audio Routing",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

private fun getDeviceIcon(type: Int): ImageVector {
    return when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET -> Icons.Default.Usb
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> Icons.Default.Headphones
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> Icons.Default.Speaker
        else -> Icons.AutoMirrored.Filled.VolumeUp
    }
}

private fun getDeviceTypeDescription(type: Int): String {
    return when (type) {
        AudioDeviceInfo.TYPE_USB_DEVICE -> "External USB Audio DAC"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "USB-C Digital Headset"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "3.5mm Analog Headphones"
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "Internal Device Speaker"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth Audio Device (A2DP)"
        else -> "Android Audio Output Device"
    }
}
