package com.vban.receptor.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Sensors
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.SettingsEthernet
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.audio.AudioDevice
import com.vban.receptor.native.NativeTelemetry
import com.vban.receptor.native.NativeVbanEngine
import com.vban.receptor.network.AutoConnectManager
import com.vban.receptor.network.DiscoveredStream
import com.vban.receptor.network.VbanDiscoveryManager
import com.vban.receptor.ui.components.CollapsibleDrawer
import com.vban.receptor.ui.components.DeviceSelector
import com.vban.receptor.ui.components.LatencyHeroDial
import com.vban.receptor.ui.components.LatencyProfileSelector
import com.vban.receptor.ui.components.MinimalStereoMeter
import com.vban.receptor.ui.components.SpectrumAnalyzer
import com.vban.receptor.ui.theme.DarkBorder
import com.vban.receptor.ui.theme.DarkSurface
import com.vban.receptor.ui.theme.DarkSurfaceVariant
import com.vban.receptor.ui.theme.HeroLabel
import com.vban.receptor.ui.theme.LiveGreen
import com.vban.receptor.ui.theme.OledBlack
import com.vban.receptor.ui.theme.StopRed
import com.vban.receptor.ui.theme.TextMuted
import com.vban.receptor.ui.theme.TextPrimary
import com.vban.receptor.ui.theme.TextSecondary
import com.vban.receptor.ui.theme.WarningAmber

/**
 * Main Bauhaus Minimalist Audiophile Dashboard for VBAN Receptor.
 *
 * Implements high-contrast pure OLED black aesthetic (#000000), stark typography,
 * real-time latency hero dial, ballistic dual micro-bars stereo meter, compact stream card,
 * zero-config one-shot auto-connect, and a collapsible advanced tools drawer.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DashboardScreen(
    onStartService: (ip: String, port: Int, stream: String, deviceId: Int) -> Unit,
    onStopService: () -> Unit,
    availableDevices: List<AudioDevice> = emptyList(),
    modifier: Modifier = Modifier
) {
    val isRunning by NativeVbanEngine.isRunning.collectAsState()
    val telemetry by NativeVbanEngine.telemetry.collectAsState()
    val detectedStreamName by NativeVbanEngine.detectedStreamName.collectAsState()
    val latencyProfile by NativeVbanEngine.latencyProfile.collectAsState()
    val spectrumBands by NativeVbanEngine.spectrumBands.collectAsState()

    val context = LocalContext.current
    val discoveryManager = remember { VbanDiscoveryManager(context = context.applicationContext) }
    val autoConnectManager = remember { AutoConnectManager() }
    val discoveredStreams by discoveryManager.discoveredStreams.collectAsState()
    val isScanning by discoveryManager.isScanning.collectAsState()

    var deviceLocalIp by remember { mutableStateOf(VbanDiscoveryManager.getLocalIpAddress(context)) }
    LaunchedEffect(Unit) {
        deviceLocalIp = VbanDiscoveryManager.getLocalIpAddress(context)
    }

    var listenIp by remember { mutableStateOf("0.0.0.0") }
    var transmitterIp by remember { mutableStateOf("") }
    var portText by remember { mutableStateOf("6980") }
    var streamName by remember { mutableStateOf("") }
    var selectedDeviceId by remember { mutableIntStateOf(0) }

    var showManualConfig by remember { mutableStateOf(false) }
    var showAdvanced by remember { mutableStateOf(false) }

    val scrollState = rememberScrollState()

    // Discovery Lifecycle: sniff when idle, cleanly release socket when disposed
    LaunchedEffect(Unit) {
        if (!isRunning) {
            discoveryManager.start()
        }
    }
    DisposableEffect(Unit) {
        onDispose {
            discoveryManager.stop()
        }
    }

    // Zero-Config Auto-Connect Execution:
    // If exactly 1 stream is discovered on LAN while in standby, auto-connect fires once.
    LaunchedEffect(discoveredStreams, isRunning) {
        val stream = autoConnectManager.shouldAutoConnect(discoveredStreams, isRunning)
        if (stream != null) {
            autoConnectManager.markAutoConnected()
            discoveryManager.pauseDiscoverySocket()
            transmitterIp = stream.sourceIp
            portText = stream.port.toString()
            streamName = stream.streamName
            onStartService("0.0.0.0", stream.port, stream.streamName, selectedDeviceId)
        }
    }

    val selectedDeviceName = availableDevices.find { it.id == selectedDeviceId }?.name

    val displayLatency = if (isRunning && telemetry.targetWatermarkMs > 0f) {
        telemetry.targetWatermarkMs
    } else if (isRunning && telemetry.sampleRate > 0) {
        (telemetry.bufferOccupancyPercent / 100f) * 20f
    } else {
        0.0f
    }

    Scaffold(
        modifier = modifier.fillMaxSize(),
        containerColor = OledBlack
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .verticalScroll(scrollState)
                .padding(horizontal = 16.dp, vertical = 20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            // 1. Top Bar: VBAN title, format subtitle, LIVE/STANDBY pill
            DashboardTopBar(
                isRunning = isRunning,
                sampleRate = telemetry.sampleRate,
                totalPackets = telemetry.totalPacketsReceived
            )

            // Local IP Address Banner with one-tap copy for Voicemeeter setup
            DeviceIpBanner(
                localIp = deviceLocalIp,
                port = portText
            )

            // 2. Real-Time Latency Hero Dial (48sp bold readout, health status caption)
            LatencyHeroDial(
                latencyMs = displayLatency,
                jitterMs = telemetry.jitterMs,
                drops = telemetry.unrecoverableDrops,
                glitches = telemetry.glitchCount,
                isRunning = isRunning
            )

            // 3. Minimalist Stereo Level Meters (6dp ballistic micro-bars with dBFS readouts)
            MinimalStereoMeter(
                leftLinear = telemetry.peakLevelLeft,
                rightLinear = telemetry.peakLevelRight,
                isRunning = isRunning
            )

            // 4. Compact Active Stream Card
            ActiveStreamCard(
                streamName = streamName,
                detectedStreamName = detectedStreamName,
                listenIp = if (transmitterIp.isNotBlank()) transmitterIp else listenIp,
                port = portText,
                telemetry = telemetry,
                deviceName = selectedDeviceName,
                isRunning = isRunning
            )

            // 5. Discovered Streams List with one-tap connect (active in standby)
            if (!isRunning) {
                DiscoveredStreamsList(
                    streams = discoveredStreams,
                    isScanning = isScanning,
                    onStreamConnect = { stream ->
                        autoConnectManager.markAutoConnected()
                        discoveryManager.pauseDiscoverySocket()
                        transmitterIp = stream.sourceIp
                        portText = stream.port.toString()
                        streamName = stream.streamName
                        onStartService("0.0.0.0", stream.port, stream.streamName, selectedDeviceId)
                    }
                )

                // 6. Manual Connection Panel (collapsible fallback)
                ManualConnectionCard(
                    listenIp = listenIp,
                    onIpChange = {
                        listenIp = it
                        transmitterIp = ""
                    },
                    portText = portText,
                    onPortChange = { portText = it },
                    streamName = streamName,
                    onStreamNameChange = { streamName = it },
                    isExpanded = showManualConfig,
                    onToggleExpand = { showManualConfig = !showManualConfig }
                )
            }

            // 7. Audio Output Routing & Bit-Perfect Badge
            DeviceSelector(
                devices = availableDevices,
                selectedDeviceId = selectedDeviceId,
                onDeviceSelected = { device ->
                    selectedDeviceId = device.id
                    NativeVbanEngine.setDevice(device.id)
                }
            )

            // 8. Collapsible Drawer housing Spectrum Analyzer & Latency Profile Selector
            CollapsibleDrawer(
                isExpanded = showAdvanced,
                onToggle = { showAdvanced = !showAdvanced },
                title = "HERRAMIENTAS AVANZADAS"
            ) {
                // 64-band real-time FFT Spectrum Analyzer
                SpectrumAnalyzer(
                    magnitudes = spectrumBands,
                    bandCount = 64,
                    isRunning = isRunning,
                    sampleRate = if (telemetry.sampleRate > 0) telemetry.sampleRate else 48000
                )

                // Adaptive Latency / Jitter Buffer Profile Selector
                LatencyProfileSelector(
                    selectedProfile = latencyProfile,
                    onProfileSelected = { NativeVbanEngine.setLatencyProfile(it) },
                    enabled = true
                )
            }

            Spacer(modifier = Modifier.height(4.dp))

            // 9. Full-Width Action Button: "DETENER RECEPCIÓN" or "INICIAR RECEPCIÓN"
            Button(
                onClick = {
                    if (isRunning) {
                        autoConnectManager.recordUserStop()
                        onStopService()
                        discoveryManager.resumeDiscoverySocket()
                    } else {
                        discoveryManager.pauseDiscoverySocket()
                        val port = portText.toIntOrNull() ?: 6980
                        val ip = if (listenIp.isBlank()) "0.0.0.0" else listenIp.trim()
                        onStartService(ip, port, streamName, selectedDeviceId)
                    }
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(54.dp),
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (isRunning) StopRed else TextPrimary,
                    contentColor = if (isRunning) TextPrimary else OledBlack
                ),
                shape = RoundedCornerShape(10.dp)
            ) {
                Icon(
                    imageVector = if (isRunning) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(20.dp)
                )
                Spacer(modifier = Modifier.width(8.dp))
                Text(
                    text = if (isRunning) "DETENER RECEPCIÓN" else "INICIAR RECEPCIÓN",
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    letterSpacing = 1.2.sp,
                    fontSize = 14.sp
                )
            }
        }
    }
}

/**
 * High-contrast Bauhaus header bar with bold VBAN branding, high-res audio badge,
 * and LIVE/STANDBY state pill.
 */
@Composable
private fun DashboardTopBar(
    isRunning: Boolean,
    sampleRate: Int,
    totalPackets: Long = 0L,
    modifier: Modifier = Modifier
) {
    val isLive = isRunning && totalPackets > 0L
    val isWaiting = isRunning && totalPackets == 0L

    val subtitleText = when {
        !isRunning -> "STANDBY"
        isWaiting -> "ESPERANDO VBAN"
        sampleRate > 0 -> "FLOAT32 ${sampleRate / 1000}K"
        else -> "FLOAT32 48K"
    }

    val pillColor = when {
        isLive -> LiveGreen
        isWaiting -> WarningAmber
        else -> TextMuted
    }

    val pillText = when {
        isLive -> "LIVE"
        isWaiting -> "ESPERANDO"
        else -> "STANDBY"
    }

    Row(
        modifier = modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column {
            Text(
                text = "VBAN",
                style = MaterialTheme.typography.titleLarge,
                fontWeight = FontWeight.Black,
                color = TextPrimary,
                letterSpacing = 2.sp
            )
            Text(
                text = subtitleText,
                style = HeroLabel,
                color = TextMuted,
                letterSpacing = 1.sp
            )
        }

        Box(
            modifier = Modifier
                .background(
                    color = pillColor.copy(alpha = 0.12f),
                    shape = RoundedCornerShape(16.dp)
                )
                .border(
                    width = 1.dp,
                    color = pillColor.copy(alpha = 0.35f),
                    shape = RoundedCornerShape(16.dp)
                )
                .padding(horizontal = 12.dp, vertical = 6.dp)
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                Box(
                    modifier = Modifier
                        .size(6.dp)
                        .background(
                            color = pillColor,
                            shape = CircleShape
                        )
                )
                Text(
                    text = pillText,
                    style = MaterialTheme.typography.labelSmall,
                    fontWeight = FontWeight.Bold,
                    color = pillColor,
                    letterSpacing = 1.sp
                )
            }
        }
    }
}

/**
 * Compact active stream telemetry card displaying stream name, endpoint,
 * format metadata, and hardware routing status.
 */
@Composable
private fun ActiveStreamCard(
    streamName: String,
    detectedStreamName: String = "",
    listenIp: String,
    port: String,
    telemetry: NativeTelemetry,
    deviceName: String?,
    isRunning: Boolean,
    modifier: Modifier = Modifier
) {
    val effectiveStreamName = when {
        detectedStreamName.isNotBlank() -> detectedStreamName
        streamName.isNotBlank() -> streamName
        else -> "Cualquier Flujo (Auto)"
    }
    val hasSignal = isRunning && telemetry.totalPacketsReceived > 0L

    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = DarkSurface),
        border = BorderStroke(1.dp, DarkBorder)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "FLUJO ACTIVO",
                    style = HeroLabel,
                    color = TextMuted,
                    letterSpacing = 1.5.sp
                )
                Text(
                    text = when {
                        !isRunning -> "DETENIDO"
                        !hasSignal -> "ESPERANDO PAQUETES"
                        else -> "AAUDIO EXCLUSIVE"
                    },
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold,
                    color = when {
                        !isRunning -> TextMuted
                        !hasSignal -> WarningAmber
                        else -> LiveGreen
                    }
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text(
                        text = effectiveStreamName,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold,
                        color = TextPrimary,
                        fontFamily = FontFamily.Monospace
                    )
                    Text(
                        text = when {
                            !isRunning -> "$listenIp:$port"
                            !hasSignal -> "$listenIp:$port · Sin señal"
                            else -> "$listenIp:$port · ${telemetry.totalPacketsReceived} pkts"
                        },
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = TextMuted
                    )
                }

                Column(horizontalAlignment = Alignment.End) {
                    val formatText = when {
                        !isRunning -> "En espera"
                        !hasSignal -> "0 Hz · 0-bit"
                        else -> "${telemetry.sampleRate} Hz · ${telemetry.bitDepth}-bit"
                    }
                    Text(
                        text = formatText,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = if (hasSignal) TextSecondary else TextMuted,
                        fontWeight = FontWeight.SemiBold
                    )
                    if (deviceName != null) {
                        Text(
                            text = deviceName,
                            style = MaterialTheme.typography.labelSmall,
                            color = TextMuted
                        )
                    }
                }
            }
        }
    }
}

/**
 * Discovered streams list card displaying zero-config VBAN streams detected on LAN
 * with one-tap connection buttons.
 */
@Composable
fun DiscoveredStreamsList(
    streams: List<DiscoveredStream>,
    isScanning: Boolean,
    onStreamConnect: (DiscoveredStream) -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = DarkSurface),
        border = BorderStroke(1.dp, DarkBorder)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.Sensors,
                        contentDescription = null,
                        tint = if (streams.isNotEmpty()) LiveGreen else TextMuted,
                        modifier = Modifier.size(16.dp)
                    )
                    Text(
                        text = if (streams.isNotEmpty()) "FLUJOS DETECTADOS (${streams.size})" else "FLUJOS DETECTADOS",
                        style = HeroLabel,
                        color = TextMuted,
                        letterSpacing = 1.5.sp
                    )
                }

                if (isScanning && streams.isEmpty()) {
                    Text(
                        text = "Escaneando...",
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                        color = TextMuted
                    )
                }
            }

            if (streams.isEmpty()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp)
                ) {
                    Text(
                        text = if (isScanning) "Buscando emisores VBAN en la red local..." else "Descubrimiento pausado",
                        style = MaterialTheme.typography.bodySmall,
                        color = TextMuted
                    )
                }
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    streams.forEach { stream ->
                        DiscoveredStreamItem(
                            stream = stream,
                            onConnect = { onStreamConnect(stream) }
                        )
                    }
                }
            }
        }
    }
}

/**
 * Individual discovered stream item row with details and one-tap 'CONECTAR' button.
 */
@Composable
private fun DiscoveredStreamItem(
    stream: DiscoveredStream,
    onConnect: () -> Unit,
    modifier: Modifier = Modifier
) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(8.dp),
        color = DarkSurfaceVariant,
        border = BorderStroke(1.dp, DarkBorder)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 14.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stream.streamName,
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    color = TextPrimary
                )
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.SettingsEthernet,
                        contentDescription = null,
                        tint = TextMuted,
                        modifier = Modifier.size(12.dp)
                    )
                    Text(
                        text = "${stream.sourceIp}:${stream.port} · ${stream.formatBadge}",
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                        color = TextMuted
                    )
                }
            }

            Button(
                onClick = onConnect,
                colors = ButtonDefaults.buttonColors(
                    containerColor = TextPrimary,
                    contentColor = OledBlack
                ),
                shape = RoundedCornerShape(6.dp),
                contentPadding = PaddingValues(horizontal = 12.dp, vertical = 6.dp)
            ) {
                Icon(
                    imageVector = Icons.Default.PlayArrow,
                    contentDescription = null,
                    modifier = Modifier.size(14.dp)
                )
                Spacer(modifier = Modifier.width(4.dp))
                Text(
                    text = "CONECTAR",
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            }
        }
    }
}

/**
 * Expandable manual configuration card allowing manual IP, Port, and Stream Name overrides.
 */
@Composable
private fun ManualConnectionCard(
    listenIp: String,
    onIpChange: (String) -> Unit,
    portText: String,
    onPortChange: (String) -> Unit,
    streamName: String,
    onStreamNameChange: (String) -> Unit,
    isExpanded: Boolean,
    onToggleExpand: () -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = DarkSurface),
        border = BorderStroke(1.dp, DarkBorder)
    ) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onToggleExpand)
                    .padding(horizontal = 20.dp, vertical = 14.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.Settings,
                        contentDescription = null,
                        tint = TextMuted,
                        modifier = Modifier.size(16.dp)
                    )
                    Text(
                        text = "CONFIGURACIÓN MANUAL",
                        style = HeroLabel,
                        color = TextMuted,
                        letterSpacing = 1.5.sp
                    )
                }

                Icon(
                    imageVector = if (isExpanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                    contentDescription = null,
                    tint = TextMuted,
                    modifier = Modifier.size(20.dp)
                )
            }

            if (isExpanded) {
                HorizontalDivider(thickness = 1.dp, color = DarkBorder)
            }

            AnimatedVisibility(
                visible = isExpanded,
                enter = expandVertically(
                    animationSpec = tween(durationMillis = 200, easing = FastOutSlowInEasing)
                ) + fadeIn(),
                exit = shrinkVertically(
                    animationSpec = tween(durationMillis = 150, easing = FastOutSlowInEasing)
                ) + fadeOut()
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 20.dp, vertical = 16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    // IP & Port
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(10.dp)
                    ) {
                        OutlinedTextField(
                            value = listenIp,
                            onValueChange = onIpChange,
                            label = { Text("IP de escucha", fontFamily = FontFamily.Monospace) },
                            supportingText = { Text("0.0.0.0 escucha en todas las interfaces", style = MaterialTheme.typography.labelSmall, color = TextMuted) },
                            modifier = Modifier.weight(2f),
                            singleLine = true,
                            colors = OutlinedTextFieldDefaults.colors(
                                focusedBorderColor = TextPrimary,
                                unfocusedBorderColor = DarkBorder,
                                focusedLabelColor = TextPrimary,
                                unfocusedLabelColor = TextMuted,
                                focusedTextColor = TextPrimary,
                                unfocusedTextColor = TextPrimary,
                                cursorColor = TextPrimary,
                                focusedContainerColor = DarkSurfaceVariant,
                                unfocusedContainerColor = DarkSurfaceVariant
                            )
                        )

                        OutlinedTextField(
                            value = portText,
                            onValueChange = onPortChange,
                            label = { Text("Puerto", fontFamily = FontFamily.Monospace) },
                            modifier = Modifier.weight(1f),
                            singleLine = true,
                            colors = OutlinedTextFieldDefaults.colors(
                                focusedBorderColor = TextPrimary,
                                unfocusedBorderColor = DarkBorder,
                                focusedLabelColor = TextPrimary,
                                unfocusedLabelColor = TextMuted,
                                focusedTextColor = TextPrimary,
                                unfocusedTextColor = TextPrimary,
                                cursorColor = TextPrimary,
                                focusedContainerColor = DarkSurfaceVariant,
                                unfocusedContainerColor = DarkSurfaceVariant
                            )
                        )
                    }

                    // Presets
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(
                            selected = listenIp == "0.0.0.0",
                            onClick = { onIpChange("0.0.0.0") },
                            label = { Text("Unicast (0.0.0.0)", fontFamily = FontFamily.Monospace, fontSize = 11.sp) },
                            colors = FilterChipDefaults.filterChipColors(
                                selectedContainerColor = TextPrimary,
                                selectedLabelColor = OledBlack,
                                containerColor = DarkSurfaceVariant,
                                labelColor = TextSecondary
                            ),
                            border = BorderStroke(1.dp, DarkBorder)
                        )
                        FilterChip(
                            selected = listenIp == "239.255.0.1",
                            onClick = { onIpChange("239.255.0.1") },
                            label = { Text("Multicast (239.255.0.1)", fontFamily = FontFamily.Monospace, fontSize = 11.sp) },
                            colors = FilterChipDefaults.filterChipColors(
                                selectedContainerColor = TextPrimary,
                                selectedLabelColor = OledBlack,
                                containerColor = DarkSurfaceVariant,
                                labelColor = TextSecondary
                            ),
                            border = BorderStroke(1.dp, DarkBorder)
                        )
                    }

                    // Stream Name
                    OutlinedTextField(
                        value = streamName,
                        onValueChange = onStreamNameChange,
                        label = { Text("Nombre del Flujo (Vacío = Aceptar Todo)", fontFamily = FontFamily.Monospace) },
                        placeholder = { Text("Cualquiera (Modo Promiscuo)", fontFamily = FontFamily.Monospace, color = TextMuted) },
                        supportingText = {
                            Text(
                                text = if (streamName.isBlank()) "Modo Promiscuo activo: aceptará cualquier flujo desde Voicemeeter" else "Filtrando por: $streamName",
                                style = MaterialTheme.typography.labelSmall,
                                color = if (streamName.isBlank()) LiveGreen else TextMuted
                            )
                        },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true,
                        colors = OutlinedTextFieldDefaults.colors(
                            focusedBorderColor = TextPrimary,
                            unfocusedBorderColor = DarkBorder,
                            focusedLabelColor = TextPrimary,
                            unfocusedLabelColor = TextMuted,
                            focusedTextColor = TextPrimary,
                            unfocusedTextColor = TextPrimary,
                            cursorColor = TextPrimary,
                            focusedContainerColor = DarkSurfaceVariant,
                            unfocusedContainerColor = DarkSurfaceVariant
                        )
                    )
                }
            }
        }
    }
}

/**
 * Prominent Local IP Banner showing the device's IP on the Wi-Fi LAN
 * with a quick copy action so the user can paste it into Voicemeeter 'IP Address To'.
 */
@Composable
fun DeviceIpBanner(
    localIp: String,
    port: String,
    modifier: Modifier = Modifier
) {
    val clipboardManager = LocalClipboardManager.current
    var copied by remember { mutableStateOf(false) }

    Surface(
        modifier = modifier
            .fillMaxWidth()
            .clickable {
                if (localIp != "0.0.0.0") {
                    clipboardManager.setText(AnnotatedString(localIp))
                    copied = true
                }
            },
        shape = RoundedCornerShape(10.dp),
        color = DarkSurface,
        border = BorderStroke(1.dp, if (copied) LiveGreen else DarkBorder)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "IP LOCAL DE ESTE DISPOSITIVO",
                    style = HeroLabel,
                    color = TextMuted,
                    letterSpacing = 1.2.sp
                )
                Spacer(modifier = Modifier.height(2.dp))
                Text(
                    text = if (localIp != "0.0.0.0") "$localIp:$port" else "0.0.0.0:$port (Conectando a Wi-Fi)",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    color = if (localIp != "0.0.0.0") TextPrimary else TextMuted
                )
                Text(
                    text = "Configura esta IP en 'IP Address To' de Voicemeeter en tu PC",
                    style = MaterialTheme.typography.labelSmall,
                    color = TextSecondary
                )
            }

            Surface(
                shape = RoundedCornerShape(6.dp),
                color = if (copied) LiveGreen.copy(alpha = 0.2f) else DarkSurfaceVariant,
                border = BorderStroke(1.dp, if (copied) LiveGreen else DarkBorder)
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    Icon(
                        imageVector = if (copied) Icons.Default.Check else Icons.Default.ContentCopy,
                        contentDescription = "Copiar IP",
                        tint = if (copied) LiveGreen else TextSecondary,
                        modifier = Modifier.size(14.dp)
                    )
                    Text(
                        text = if (copied) "COPIADO" else "COPIAR",
                        style = MaterialTheme.typography.labelSmall,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold,
                        color = if (copied) LiveGreen else TextSecondary
                    )
                }
            }
        }
    }
}
