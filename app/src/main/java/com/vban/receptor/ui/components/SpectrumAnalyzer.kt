package com.vban.receptor.ui.components

import androidx.compose.foundation.BorderStroke
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.GraphicEq
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
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
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.ui.theme.CrimsonRed
import com.vban.receptor.ui.theme.CyanAccent
import com.vban.receptor.ui.theme.EmeraldGreen
import com.vban.receptor.ui.theme.GoldAccent
import com.vban.receptor.ui.theme.OutlineDark
import com.vban.receptor.ui.theme.SurfaceDark
import com.vban.receptor.ui.theme.SurfaceVariantDark
import kotlinx.coroutines.isActive
import kotlin.math.max
import kotlin.math.min

// Audiophile Spectrum Visualizer Constants
private val AnalyzerBackground = Color(0xFF090E17)
private val GridLineColor = Color(0x22475569)
private val PeakHoldTickColor = Color(0xFFFFFFFF)

/**
 * High-performance 60 FPS Material 3 Canvas visualizer displaying 32 or 64 logarithmic
 * frequency bands with animated gradient bars and DIN/EBU peak hold decay.
 *
 * @param magnitudes Normalized magnitude spectrum [0.0f, 1.0f] for display bands.
 * @param bandCount Number of display bands (typically 32 or 64).
 * @param isRunning True if audio is streaming; false renders idle standby floor.
 * @param sampleRate Current audio sample rate in Hz (e.g. 48000, 96000, 192000).
 * @param modifier Optional [Modifier].
 */
@Composable
fun SpectrumAnalyzer(
    magnitudes: FloatArray,
    bandCount: Int = 64,
    isRunning: Boolean = true,
    sampleRate: Int = 48000,
    modifier: Modifier = Modifier
) {
    val effectiveBands = bandCount.coerceIn(16, 64)

    // Ballistic animated state arrays
    var currentLevels by remember(effectiveBands) { mutableStateOf(FloatArray(effectiveBands)) }
    var peakHolds by remember(effectiveBands) { mutableStateOf(FloatArray(effectiveBands)) }
    var peakHoldTimers by remember(effectiveBands) { mutableStateOf(LongArray(effectiveBands)) }

    val latestMagnitudes by rememberUpdatedState(magnitudes)
    val latestIsRunning by rememberUpdatedState(isRunning)

    // 60 FPS Dynamic Animation Ballistic Loop
    LaunchedEffect(isRunning, effectiveBands) {
        if (!isRunning) {
            currentLevels = FloatArray(effectiveBands)
            peakHolds = FloatArray(effectiveBands)
            peakHoldTimers = LongArray(effectiveBands)
            return@LaunchedEffect
        }

        var lastFrameNanos = 0L
        val decayRate = 2.4f      // Linear decay units per second
        val peakDecayRate = 1.2f  // Peak hold decay units per second
        val peakHoldDurationMs = 600L

        while (isActive) {
            withFrameNanos { frameNanos ->
                if (lastFrameNanos == 0L) {
                    lastFrameNanos = frameNanos
                    return@withFrameNanos
                }

                val dtSec = ((frameNanos - lastFrameNanos) / 1_000_000_000f).coerceIn(0.001f, 0.1f)
                lastFrameNanos = frameNanos
                val nowMs = frameNanos / 1_000_000L

                val nextLevels = currentLevels.copyOf()
                val nextPeakHolds = peakHolds.copyOf()
                val nextPeakTimers = peakHoldTimers.copyOf()
                val input = latestMagnitudes

                for (b in 0 until effectiveBands) {
                    val rawVal = if (latestIsRunning && b < input.size) {
                        input[b].coerceIn(0.0f, 1.0f)
                    } else {
                        0.0f
                    }

                    // Instantaneous fast attack, smooth exponential/linear decay
                    if (rawVal >= nextLevels[b]) {
                        nextLevels[b] = rawVal
                    } else {
                        nextLevels[b] = max(0.0f, nextLevels[b] - decayRate * dtSec)
                    }

                    // Peak Hold Ballistics: freeze for hold duration, then decay smoothly
                    if (nextLevels[b] >= nextPeakHolds[b]) {
                        nextPeakHolds[b] = nextLevels[b]
                        nextPeakTimers[b] = nowMs + peakHoldDurationMs
                    } else if (nowMs > nextPeakTimers[b]) {
                        nextPeakHolds[b] = max(0.0f, nextPeakHolds[b] - peakDecayRate * dtSec)
                    }
                }

                currentLevels = nextLevels
                peakHolds = nextPeakHolds
                peakHoldTimers = nextPeakTimers
            }
        }
    }

    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = SurfaceDark),
        border = BorderStroke(1.dp, OutlineDark)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            // Header Bar: Title, Band Count & Nyquist Bandwidth
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
                        imageVector = Icons.Default.GraphicEq,
                        contentDescription = "FFT Spectrum Analyzer",
                        tint = CyanAccent,
                        modifier = Modifier.size(18.dp)
                    )
                    Text(
                        text = "REAL-TIME SPECTRUM ANALYZER",
                        style = MaterialTheme.typography.labelMedium.copy(
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 1.2.sp
                        ),
                        color = Color.White
                    )
                }

                Surface(
                    shape = RoundedCornerShape(6.dp),
                    color = CyanAccent.copy(alpha = 0.15f),
                    border = BorderStroke(1.dp, CyanAccent.copy(alpha = 0.35f))
                ) {
                    Text(
                        text = "$effectiveBands BANDS · 1024-PT FFT",
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
                        style = MaterialTheme.typography.labelSmall.copy(
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.Bold,
                            fontSize = 10.sp
                        ),
                        color = CyanAccent
                    )
                }
            }

            // 60 FPS Visualizer Canvas
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(140.dp)
                    .background(AnalyzerBackground, RoundedCornerShape(8.dp))
                    .padding(horizontal = 8.dp, vertical = 8.dp)
            ) {
                Canvas(modifier = Modifier.matchParentSize()) {
                    val canvasWidth = size.width
                    val canvasHeight = size.height

                    // 1. Draw subtle horizontal dBFS grid lines (-6dB, -18dB, -36dB, -60dB)
                    val dbGridFractions = floatArrayOf(0.0f, 0.25f, 0.50f, 0.75f, 1.0f)
                    for (frac in dbGridFractions) {
                        val y = canvasHeight * (1.0f - frac)
                        drawLine(
                            color = GridLineColor,
                            start = Offset(0f, y),
                            end = Offset(canvasWidth, y),
                            strokeWidth = 1f
                        )
                    }

                    // 2. Bar geometry calculation
                    val totalBars = effectiveBands
                    val spacing = if (totalBars > 32) 1.5f else 2.5f
                    val totalSpacing = spacing * (totalBars - 1)
                    val barWidth = max(1.0f, (canvasWidth - totalSpacing) / totalBars)

                    // Multi-stop Audiophile Gradient (Cyan -> Emerald -> Gold -> Crimson)
                    val barGradient = Brush.verticalGradient(
                        colors = listOf(
                            CrimsonRed,   // Peak / Near 0 dBFS (Top)
                            GoldAccent,   // -6 dBFS
                            EmeraldGreen, // -18 dBFS
                            CyanAccent    // Baseline / Low levels (Bottom)
                        ),
                        startY = 0f,
                        endY = canvasHeight
                    )

                    // 3. Render animated gradient bars with peak hold tick caps
                    val cornerRad = CornerRadius(barWidth * 0.4f, barWidth * 0.4f)
                    val minBarHeight = 2f

                    for (i in 0 until totalBars) {
                        val x = i * (barWidth + spacing)
                        val level = currentLevels.getOrElse(i) { 0.0f }
                        val peak = peakHolds.getOrElse(i) { 0.0f }

                        val barHeight = max(minBarHeight, level * canvasHeight)
                        val barTop = canvasHeight - barHeight

                        // Render gradient magnitude bar
                        drawRoundRect(
                            brush = barGradient,
                            topLeft = Offset(x, barTop),
                            size = Size(barWidth, barHeight),
                            cornerRadius = cornerRad
                        )

                        // Render white/cyan peak hold cap tick
                        if (peak > 0.03f) {
                            val peakY = canvasHeight - (peak * canvasHeight).coerceAtMost(canvasHeight - 2f)
                            drawRoundRect(
                                color = PeakHoldTickColor,
                                topLeft = Offset(x, peakY),
                                size = Size(barWidth, 2f),
                                cornerRadius = CornerRadius(1f, 1f)
                            )
                        }
                    }
                }
            }

            // Frequency Scale Axis Labels (20 Hz to 20 kHz / Nyquist)
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 4.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                val nyquistKhz = (sampleRate / 2000)
                val labels = listOf("20Hz", "100Hz", "500Hz", "1kHz", "5kHz", "10kHz", "${nyquistKhz}kHz")
                labels.forEach { label ->
                    Text(
                        text = label,
                        style = MaterialTheme.typography.labelSmall.copy(
                            fontFamily = FontFamily.Monospace,
                            fontSize = 9.sp
                        ),
                        color = Color(0xFF64748B)
                    )
                }
            }
        }
    }
}
