package com.vban.receptor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.native.NativeTelemetry
import com.vban.receptor.native.NativeVbanEngine
import com.vban.receptor.ui.components.AudioOutputDeviceItem
import com.vban.receptor.ui.components.BufferOccupancyGauge
import com.vban.receptor.ui.components.DeviceSelector
import com.vban.receptor.ui.components.StereoVuMeter
import java.util.Locale

/**
 * Main Audiophile Dashboard Screen for VBAN Receptor.
 */
@Composable
fun DashboardScreen(
    onStartService: (ip: String, port: Int, stream: String, deviceId: Int) -> Unit,
    onStopService: () -> Unit,
    availableDevices: List<AudioOutputDeviceItem> = emptyList(),
    modifier: Modifier = Modifier
) {
    val isRunning by NativeVbanEngine.isRunning.collectAsState()
    val telemetry by NativeVbanEngine.telemetry.collectAsState()

    var listenIp by remember { mutableStateOf("0.0.0.0") }
    var portText by remember { mutableStateOf("6980") }
    var streamName by remember { mutableStateOf("Stream1") }
    var selectedDeviceId by remember { mutableIntStateOf(0) }

    val scrollState = rememberScrollState()

    Scaffold(
        modifier = modifier.fillMaxSize(),
        containerColor = MaterialTheme.colorScheme.background
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(scrollState)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            // Header Bar
            DashboardHeader(isRunning = isRunning)

            // Connection Configuration Card
            ConnectionConfigCard(
                listenIp = listenIp,
                onIpChanged = { listenIp = it },
                portText = portText,
                onPortChanged = { portText = it },
                streamName = streamName,
                onStreamNameChanged = { streamName = it },
                isRunning = isRunning,
                onTogglePlayback = {
                    if (isRunning) {
                        onStopService()
                    } else {
                        val port = portText.toIntOrNull() ?: 6980
                        onStartService(listenIp, port, streamName, selectedDeviceId)
                    }
                }
            )

            // Audio Output Routing & Bit-Perfect Badge
            DeviceSelector(
                devices = availableDevices,
                selectedDeviceId = selectedDeviceId,
                onDeviceSelected = { device ->
                    selectedDeviceId = device.id
                    NativeVbanEngine.setDevice(device.id)
                }
            )

            // 60 FPS Stereo VU Meter
            StereoVuMeter(
                leftLinear = telemetry.peakLevelLeft,
                rightLinear = telemetry.peakLevelRight,
                isRunning = isRunning
            )

            // Buffer Watermark Gauge & Stream Telemetry
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                BufferOccupancyGauge(
                    occupancyPercent = telemetry.bufferOccupancyPercent,
                    sampleRate = telemetry.sampleRate,
                    jitterMs = telemetry.jitterMs,
                    modifier = Modifier.weight(1f)
                )

                StreamTelemetryCard(
                    telemetry = telemetry,
                    isRunning = isRunning,
                    modifier = Modifier.weight(1f)
                )
            }
        }
    }
}

@Composable
private fun DashboardHeader(isRunning: Boolean) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column {
            Text(
                text = "VBAN RECEPTOR",
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.ExtraBold,
                color = MaterialTheme.colorScheme.onBackground,
                letterSpacing = 1.5.sp
            )
            Text(
                text = "High-Res Float32 Low-Latency Audio",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.primary
            )
        }

        Box(
            modifier = Modifier
                .background(
                    color = if (isRunning) Color(0xFF00E676).copy(alpha = 0.2f) else Color(0xFF334155),
                    shape = RoundedCornerShape(16.dp)
                )
                .padding(horizontal = 12.dp, vertical = 6.dp)
        ) {
            Text(
                text = if (isRunning) "STREAMING ACTIVE" else "STANDBY",
                style = MaterialTheme.typography.labelSmall,
                fontWeight = FontWeight.Bold,
                color = if (isRunning) Color(0xFF00E676) else Color(0xFF94A3B8)
            )
        }
    }
}

@Composable
private fun ConnectionConfigCard(
    listenIp: String,
    onIpChanged: (String) -> Unit,
    portText: String,
    onPortChanged: (String) -> Unit,
    streamName: String,
    onStreamNameChanged: (String) -> Unit,
    isRunning: Boolean,
    onTogglePlayback: () -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "NETWORK STREAM CONFIGURATION",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(12.dp))

            // IP & Port Inputs
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                OutlinedTextField(
                    value = listenIp,
                    onValueChange = onIpChanged,
                    label = { Text("Listen IP") },
                    enabled = !isRunning,
                    modifier = Modifier.weight(2f),
                    singleLine = true,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = MaterialTheme.colorScheme.primary,
                        unfocusedBorderColor = MaterialTheme.colorScheme.outline
                    )
                )

                OutlinedTextField(
                    value = portText,
                    onValueChange = onPortChanged,
                    label = { Text("UDP Port") },
                    enabled = !isRunning,
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = MaterialTheme.colorScheme.primary,
                        unfocusedBorderColor = MaterialTheme.colorScheme.outline
                    )
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // IP Presets (Unicast / Multicast)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(
                    selected = listenIp == "0.0.0.0",
                    onClick = { onIpChanged("0.0.0.0") },
                    label = { Text("Unicast (0.0.0.0)") },
                    enabled = !isRunning
                )
                FilterChip(
                    selected = listenIp == "239.255.0.1",
                    onClick = { onIpChanged("239.255.0.1") },
                    label = { Text("Multicast (239.255.0.1)") },
                    enabled = !isRunning
                )
            }

            Spacer(modifier = Modifier.height(8.dp))

            // Stream Name Input
            OutlinedTextField(
                value = streamName,
                onValueChange = onStreamNameChanged,
                label = { Text("Stream Name (VBAN Header Filter)") },
                enabled = !isRunning,
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                colors = OutlinedTextFieldDefaults.colors(
                    focusedBorderColor = MaterialTheme.colorScheme.primary,
                    unfocusedBorderColor = MaterialTheme.colorScheme.outline
                )
            )

            Spacer(modifier = Modifier.height(14.dp))

            // Primary Playback Toggle Button
            Button(
                onClick = onTogglePlayback,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(50.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (isRunning) Color(0xFFEF4444) else MaterialTheme.colorScheme.primary
                ),
                shape = RoundedCornerShape(8.dp)
            ) {
                Icon(
                    imageVector = if (isRunning) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(22.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    text = if (isRunning) "STOP RECEPTOR" else "START RECEPTOR",
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.0.sp
                )
            }
        }
    }
}

@Composable
private fun StreamTelemetryCard(
    telemetry: NativeTelemetry,
    isRunning: Boolean,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Text(
                text = "STREAM TELEMETRY",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(12.dp))

            TelemetryItem(
                label = "Sample Rate",
                value = if (isRunning && telemetry.sampleRate > 0) "${telemetry.sampleRate} Hz" else "---"
            )
            TelemetryItem(
                label = "Channels",
                value = if (isRunning && telemetry.channelCount > 0) "${telemetry.channelCount} ch" else "---"
            )
            TelemetryItem(
                label = "Bit Depth",
                value = if (isRunning && telemetry.bitDepth > 0) "${telemetry.bitDepth}-bit" else "---"
            )
            TelemetryItem(
                label = "Packets",
                value = if (isRunning) "${telemetry.totalPacketsReceived}" else "---"
            )
            TelemetryItem(
                label = "Loss Rate",
                value = if (isRunning) String.format(Locale.US, "%.2f%%", telemetry.packetLossRatePercent) else "---",
                isWarning = telemetry.packetLossRatePercent > 0.1f
            )
        }
    }
}

@Composable
private fun TelemetryItem(
    label: String,
    value: String,
    isWarning: Boolean = false
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.SemiBold,
            color = if (isWarning) Color(0xFFEF4444) else MaterialTheme.colorScheme.onSurface
        )
    }
}
