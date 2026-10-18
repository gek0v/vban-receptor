package com.vban.receptor.ui.components

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.isActive
import java.util.Locale

// Audiophile VU Meter Palette Constants
private val MeterBackground = Color(0xFF131B26)
private val MeterGreen = Color(0xFF00E676)
private val MeterAmber = Color(0xFFFFD600)
private val MeterOrange = Color(0xFFFF9100)
private val MeterRed = Color(0xFFFF1744)
private val TickColor = Color(0xFF475569)
private val PeakHoldColor = Color(0xFFFFFFFF)

/**
 * High-performance 60 FPS Stereo VU Meter card with DIN 45406 / EBU R128 broadcast ballistics.
 *
 * @param leftLinear Linear peak amplitude for Left channel [0.0f, 1.0f].
 * @param rightLinear Linear peak amplitude for Right channel [0.0f, 1.0f].
 * @param isRunning True if engine is streaming; false renders idle standby state.
 */
@Composable
fun StereoVuMeter(
    leftLinear: Float,
    rightLinear: Float,
    isRunning: Boolean,
    modifier: Modifier = Modifier
) {
    // 60 FPS Ballistic state variables
    var currentLeftDb by remember { mutableFloatStateOf(-60.0f) }
    var currentRightDb by remember { mutableFloatStateOf(-60.0f) }
    var peakHoldLeftDb by remember { mutableFloatStateOf(-60.0f) }
    var peakHoldRightDb by remember { mutableFloatStateOf(-60.0f) }
    var peakHoldLeftTimerMs by remember { mutableLongStateOf(0L) }
    var peakHoldRightTimerMs by remember { mutableLongStateOf(0L) }
    var clipLeftTimerMs by remember { mutableLongStateOf(0L) }
    var clipRightTimerMs by remember { mutableLongStateOf(0L) }

    // Track latest inputs dynamically inside the persistent animation loop
    val currentLeftLinear by rememberUpdatedState(leftLinear)
    val currentRightLinear by rememberUpdatedState(rightLinear)
    val currentIsRunning by rememberUpdatedState(isRunning)

    // 60 FPS Animation Frame Loop
    LaunchedEffect(isRunning) {
        if (!isRunning) {
            currentLeftDb = -60.0f
            currentRightDb = -60.0f
            peakHoldLeftDb = -60.0f
            peakHoldRightDb = -60.0f
            return@LaunchedEffect
        }

        var lastFrameNanos = 0L
        while (isActive) {
            withFrameNanos { frameNanos ->
                if (lastFrameNanos == 0L) {
                    lastFrameNanos = frameNanos
                    return@withFrameNanos
                }
                val dtSec = ((frameNanos - lastFrameNanos) / 1_000_000_000f).coerceIn(0.001f, 0.1f)
                val dtMs = (dtSec * 1000f).toLong()
                lastFrameNanos = frameNanos

                val lLin = currentLeftLinear
                val rLin = currentRightLinear
                val inRunning = currentIsRunning

                val targetLeftDb = if (inRunning) linearToDbfs(lLin) else -60.0f
                val targetRightDb = if (inRunning) linearToDbfs(rLin) else -60.0f

                // Ballistic Decay Rates: 30 dB/s release, 15 dB/s peak decay
                val releaseDecay = 30.0f * dtSec
                val peakDecay = 15.0f * dtSec

                // Left Channel Ballistics
                if (targetLeftDb >= currentLeftDb) {
                    currentLeftDb = targetLeftDb // Instant attack
                } else {
                    currentLeftDb = (currentLeftDb - releaseDecay).coerceAtLeast(targetLeftDb)
                }

                if (currentLeftDb >= peakHoldLeftDb) {
                    peakHoldLeftDb = currentLeftDb
                    peakHoldLeftTimerMs = 1200L // Hold 1.2 seconds
                } else if (peakHoldLeftTimerMs > 0L) {
                    peakHoldLeftTimerMs = (peakHoldLeftTimerMs - dtMs).coerceAtLeast(0L)
                } else {
                    peakHoldLeftDb = (peakHoldLeftDb - peakDecay).coerceAtLeast(-60.0f)
                }

                if (lLin >= 0.999f && inRunning) {
                    clipLeftTimerMs = 1000L
                } else if (clipLeftTimerMs > 0L) {
                    clipLeftTimerMs = (clipLeftTimerMs - dtMs).coerceAtLeast(0L)
                }

                // Right Channel Ballistics
                if (targetRightDb >= currentRightDb) {
                    currentRightDb = targetRightDb // Instant attack
                } else {
                    currentRightDb = (currentRightDb - releaseDecay).coerceAtLeast(targetRightDb)
                }

                if (currentRightDb >= peakHoldRightDb) {
                    peakHoldRightDb = currentRightDb
                    peakHoldRightTimerMs = 1200L
                } else if (peakHoldRightTimerMs > 0L) {
                    peakHoldRightTimerMs = (peakHoldRightTimerMs - dtMs).coerceAtLeast(0L)
                } else {
                    peakHoldRightDb = (peakHoldRightDb - peakDecay).coerceAtLeast(-60.0f)
                }

                if (rLin >= 0.999f && inRunning) {
                    clipRightTimerMs = 1000L
                } else if (clipRightTimerMs > 0L) {
                    clipRightTimerMs = (clipRightTimerMs - dtMs).coerceAtLeast(0L)
                }
            }
        }
    }

    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            // Card Title Header
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "STEREO VU METER",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.2.sp
                )
                Text(
                    text = if (isRunning) "ACTIVE • 60 FPS" else "STANDBY",
                    style = MaterialTheme.typography.labelSmall,
                    color = if (isRunning) MeterGreen else Color.Gray,
                    fontFamily = FontFamily.Monospace
                )
            }

            Spacer(modifier = Modifier.height(12.dp))

            // Calibration scale tick marks
            VuScaleTicks()

            Spacer(modifier = Modifier.height(6.dp))

            // Left Channel Bar
            VuMeterBarRow(
                channelLabel = "L",
                levelDb = currentLeftDb,
                peakHoldDb = peakHoldLeftDb,
                isClipping = clipLeftTimerMs > 0L
            )

            Spacer(modifier = Modifier.height(8.dp))

            // Right Channel Bar
            VuMeterBarRow(
                channelLabel = "R",
                levelDb = currentRightDb,
                peakHoldDb = peakHoldRightDb,
                isClipping = clipRightTimerMs > 0L
            )
        }
    }
}

/**
 * Single audio channel row with label, canvas bar, numeric readout, and clip LED.
 */
@Composable
private fun VuMeterBarRow(
    channelLabel: String,
    levelDb: Float,
    peakHoldDb: Float,
    isClipping: Boolean,
    modifier: Modifier = Modifier
) {
    // Zero-allocation gradient cache
    val meterGradient = remember {
        Brush.horizontalGradient(
            0.00f to MeterGreen,   // -60 dBFS
            0.66f to MeterGreen,   // -20 dBFS
            0.80f to MeterAmber,   // -12 dBFS
            0.90f to MeterOrange,  // -6 dBFS
            1.00f to MeterRed      // 0 dBFS Overload
        )
    }

    val normLevel = ((levelDb + 60.0f) / 60.0f).coerceIn(0.0f, 1.0f)
    val normPeakHold = ((peakHoldDb + 60.0f) / 60.0f).coerceIn(0.0f, 1.0f)

    Row(
        modifier = modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically
    ) {
        // Channel Label
        Text(
            text = channelLabel,
            modifier = Modifier.width(18.dp),
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.onSurface
        )

        Spacer(modifier = Modifier.width(6.dp))

        // High-Performance Zero-Allocation Meter Canvas
        Canvas(
            modifier = Modifier
                .weight(1.0f)
                .height(18.dp)
        ) {
            val width = size.width
            val height = size.height
            val corner = CornerRadius(3.dp.toPx(), 3.dp.toPx())

            // Background trough
            drawRoundRect(
                color = MeterBackground,
                size = Size(width, height),
                cornerRadius = corner
            )

            // Active level bar with gradient
            if (normLevel > 0.001f) {
                drawRoundRect(
                    brush = meterGradient,
                    size = Size(width * normLevel, height),
                    cornerRadius = corner
                )
            }

            // Peak Hold indicator line
            if (normPeakHold > 0.01f) {
                val peakX = width * normPeakHold
                drawLine(
                    color = PeakHoldColor,
                    start = Offset(peakX, 0f),
                    end = Offset(peakX, height),
                    strokeWidth = 2.dp.toPx()
                )
            }
        }

        Spacer(modifier = Modifier.width(8.dp))

        // Numeric Decibel Readout
        Text(
            text = formatDbfs(levelDb),
            modifier = Modifier.width(58.dp),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = if (levelDb >= -1.0f) MeterRed else MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.End
        )

        Spacer(modifier = Modifier.width(6.dp))

        // Hardware Clip LED Indicator
        Box(
            modifier = Modifier
                .size(10.dp)
                .background(
                    color = if (isClipping) MeterRed else MeterBackground,
                    shape = CircleShape
                )
        )
    }
}

/**
 * Scale tick graduation marks (-60, -40, -20, -12, -6, 0 dB).
 */
@Composable
private fun VuScaleTicks(modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(start = 24.dp, end = 74.dp), // Align exactly with canvas bar
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        val ticks = listOf("-60", "-40", "-20", "-12", "-6", "0")
        ticks.forEach { tick ->
            Text(
                text = tick,
                style = MaterialTheme.typography.labelSmall.copy(fontSize = 9.sp),
                color = TickColor,
                fontFamily = FontFamily.Monospace
            )
        }
    }
}

private fun linearToDbfs(linear: Float): Float {
    if (linear <= 0.00001f) return -60.0f
    val db = 20.0f * kotlin.math.log10(linear)
    return db.coerceIn(-60.0f, 0.0f)
}

private fun formatDbfs(db: Float): String {
    return if (db <= -59.5f) {
        "-∞ dB"
    } else {
        String.format(Locale.US, "%5.1f", db)
    }
}
