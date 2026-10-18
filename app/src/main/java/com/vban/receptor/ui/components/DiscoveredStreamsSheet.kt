package com.vban.receptor.ui.components

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Sensors
import androidx.compose.material.icons.filled.SettingsEthernet
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SheetState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.network.DiscoveredStream
import com.vban.receptor.ui.theme.CyanAccent
import com.vban.receptor.ui.theme.EmeraldGreen
import com.vban.receptor.ui.theme.GoldAccent
import com.vban.receptor.ui.theme.OnSurfaceLight
import com.vban.receptor.ui.theme.OnSurfaceMuted
import com.vban.receptor.ui.theme.OutlineDark
import com.vban.receptor.ui.theme.SurfaceDark
import com.vban.receptor.ui.theme.SurfaceVariantDark

/**
 * Material 3 Modal Bottom Sheet displaying zero-config VBAN streams discovered on the network.
 *
 * Provides real-time stream list, audio format badges (e.g. '96 kHz | 24-bit | Stereo'),
 * direct 'Conectar' action buttons, and a pulsing radar scanning animation when waiting for streams.
 *
 * @param streams Active list of detected [DiscoveredStream] instances.
 * @param onStreamSelected Callback invoked with the selected stream to initiate connection.
 * @param onDismissRequest Callback invoked when the user dismisses the bottom sheet.
 * @param isScanning Whether the discovery manager is actively sniffing network packets.
 * @param sheetState Bottom sheet state controlling modal presentation.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DiscoveredStreamsSheet(
    streams: List<DiscoveredStream>,
    onStreamSelected: (DiscoveredStream) -> Unit,
    onDismissRequest: () -> Unit,
    modifier: Modifier = Modifier,
    isScanning: Boolean = true,
    sheetState: SheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
) {
    ModalBottomSheet(
        onDismissRequest = onDismissRequest,
        sheetState = sheetState,
        containerColor = SurfaceDark,
        scrimColor = Color.Black.copy(alpha = 0.65f),
        shape = RoundedCornerShape(topStart = 24.dp, topEnd = 24.dp),
        dragHandle = null,
        modifier = modifier
    ) {
        DiscoveredStreamsContent(
            streams = streams,
            onStreamSelected = onStreamSelected,
            onDismiss = onDismissRequest,
            isScanning = isScanning
        )
    }
}

/**
 * Main content layout for discovered streams, reusable inside dialogs, bottom sheets, or screens.
 */
@Composable
fun DiscoveredStreamsContent(
    streams: List<DiscoveredStream>,
    onStreamSelected: (DiscoveredStream) -> Unit,
    onDismiss: () -> Unit,
    modifier: Modifier = Modifier,
    isScanning: Boolean = true
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .padding(horizontal = 20.dp, vertical = 16.dp)
    ) {
        // Sheet Header
        DiscoveredStreamsHeader(
            streamCount = streams.size,
            isScanning = isScanning,
            onDismiss = onDismiss
        )

        Spacer(modifier = Modifier.height(16.dp))

        if (streams.isEmpty()) {
            EmptyScanningState(
                isScanning = isScanning,
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 32.dp)
            )
        } else {
            LazyColumn(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 4.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
                contentPadding = PaddingValues(bottom = 24.dp)
            ) {
                items(
                    items = streams,
                    key = { it.id }
                ) { stream ->
                    DiscoveredStreamCard(
                        stream = stream,
                        onConnect = { onStreamSelected(stream) }
                    )
                }
            }
        }
    }
}

@Composable
private fun DiscoveredStreamsHeader(
    streamCount: Int,
    isScanning: Boolean,
    onDismiss: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(36.dp)
                    .background(CyanAccent.copy(alpha = 0.15f), shape = RoundedCornerShape(10.dp)),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Default.Sensors,
                    contentDescription = null,
                    tint = CyanAccent,
                    modifier = Modifier.size(20.dp)
                )
            }

            Column {
                Text(
                    text = "Flujos VBAN Detectados",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = OnSurfaceLight
                )
                Text(
                    text = if (isScanning) {
                        if (streamCount == 1) "1 flujo activo en puerto 6980"
                        else "$streamCount flujos activos en puerto 6980"
                    } else {
                        "Descubrimiento pausado"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = OnSurfaceMuted
                )
            }
        }

        IconButton(onClick = onDismiss) {
            Icon(
                imageVector = Icons.Default.Close,
                contentDescription = "Cerrar",
                tint = OnSurfaceMuted
            )
        }
    }
}

/**
 * High-tech individual stream card displaying stream identity, network address,
 * audio parameters badge ('96 kHz | 24-bit | Stereo'), and 'Conectar' action button.
 */
@Composable
fun DiscoveredStreamCard(
    stream: DiscoveredStream,
    onConnect: () -> Unit,
    modifier: Modifier = Modifier
) {
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = SurfaceVariantDark),
        shape = RoundedCornerShape(14.dp),
        border = CardDefaults.outlinedCardBorder().copy(
            brush = Brush.horizontalGradient(
                listOf(OutlineDark, CyanAccent.copy(alpha = 0.25f))
            )
        )
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Top Row: Stream Name & Connection Source
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.Top
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = stream.streamName,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold,
                        color = OnSurfaceLight,
                        fontFamily = FontFamily.Monospace,
                        letterSpacing = 0.5.sp
                    )

                    Spacer(modifier = Modifier.height(4.dp))

                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(4.dp)
                    ) {
                        Icon(
                            imageVector = Icons.Default.SettingsEthernet,
                            contentDescription = null,
                            tint = OnSurfaceMuted,
                            modifier = Modifier.size(14.dp)
                        )
                        Text(
                            text = "${stream.sourceIp}:${stream.port}",
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = OnSurfaceMuted
                        )
                    }
                }

                // Audio format badge (e.g. '96 kHz | 24-bit | Stereo')
                AudioFormatBadge(badgeText = stream.formatBadge)
            }

            // Bottom Row: Status Indicator & 'Conectar' Button
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                // Live activity indicator
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(8.dp)
                            .background(EmeraldGreen, shape = CircleShape)
                    )
                    Text(
                        text = "Activo",
                        style = MaterialTheme.typography.labelSmall,
                        fontWeight = FontWeight.SemiBold,
                        color = EmeraldGreen
                    )
                }

                // Primary Connection Action Button
                Button(
                    onClick = onConnect,
                    shape = RoundedCornerShape(8.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = CyanAccent,
                        contentColor = Color.Black
                    ),
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp)
                ) {
                    Icon(
                        imageVector = Icons.Default.PlayArrow,
                        contentDescription = null,
                        modifier = Modifier.size(18.dp)
                    )
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(
                        text = "Conectar",
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 0.5.sp
                    )
                }
            }
        }
    }
}

/**
 * High-resolution audio badge displaying sample rate, bit depth, and channels.
 */
@Composable
fun AudioFormatBadge(
    badgeText: String,
    modifier: Modifier = Modifier
) {
    Surface(
        modifier = modifier,
        color = CyanAccent.copy(alpha = 0.12f),
        shape = RoundedCornerShape(8.dp),
        border = null
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Icon(
                imageVector = Icons.Default.GraphicEq,
                contentDescription = null,
                tint = CyanAccent,
                modifier = Modifier.size(14.dp)
            )
            Text(
                text = badgeText,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
                color = CyanAccent,
                letterSpacing = 0.5.sp
            )
        }
    }
}

/**
 * Animated radar scanning state displayed when zero streams have been detected yet.
 */
@Composable
fun EmptyScanningState(
    isScanning: Boolean,
    modifier: Modifier = Modifier
) {
    val infiniteTransition = rememberInfiniteTransition(label = "RadarScanning")

    val pulseScale by infiniteTransition.animateFloat(
        initialValue = 0.7f,
        targetValue = 1.35f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 1800, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Restart
        ),
        label = "PulseScale"
    )

    val pulseAlpha by infiniteTransition.animateFloat(
        initialValue = 0.6f,
        targetValue = 0.0f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 1800, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Restart
        ),
        label = "PulseAlpha"
    )

    Column(
        modifier = modifier,
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        // Radar Pulsing Circle Visual
        Box(
            modifier = Modifier.size(110.dp),
            contentAlignment = Alignment.Center
        ) {
            if (isScanning) {
                // Expanding outer ring
                Box(
                    modifier = Modifier
                        .size(90.dp)
                        .scale(pulseScale)
                        .clip(CircleShape)
                        .background(CyanAccent.copy(alpha = pulseAlpha))
                )

                // Secondary inner glow ring
                Box(
                    modifier = Modifier
                        .size(70.dp)
                        .scale(pulseScale * 0.85f)
                        .clip(CircleShape)
                        .border(1.5.dp, CyanAccent.copy(alpha = pulseAlpha * 0.8f), CircleShape)
                )
            }

            // Core center radar icon circle
            Box(
                modifier = Modifier
                    .size(54.dp)
                    .background(SurfaceVariantDark, shape = CircleShape)
                    .border(1.5.dp, if (isScanning) CyanAccent else OutlineDark, CircleShape),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    imageVector = Icons.Default.Sensors,
                    contentDescription = null,
                    tint = if (isScanning) CyanAccent else OnSurfaceMuted,
                    modifier = Modifier.size(28.dp)
                )
            }
        }

        Spacer(modifier = Modifier.height(20.dp))

        Text(
            text = if (isScanning) "Buscando flujos VBAN..." else "Descubrimiento en espera",
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.Bold,
            color = OnSurfaceLight
        )

        Spacer(modifier = Modifier.height(6.dp))

        Text(
            text = if (isScanning) {
                "Escuchando paquetes UDP en el puerto 6980.\nAsegúrate de que el transmisor VBAN esté emitiendo."
            } else {
                "Inicia el receptor o activa la búsqueda de red."
            },
            style = MaterialTheme.typography.bodySmall,
            color = OnSurfaceMuted,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            modifier = Modifier.padding(horizontal = 24.dp)
        )
    }
}
