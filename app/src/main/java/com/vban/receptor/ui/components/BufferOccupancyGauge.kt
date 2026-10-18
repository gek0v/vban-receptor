package com.vban.receptor.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale

private val ZoneOptimal = Color(0xFF10B981)
private val ZoneWarning = Color(0xFFF59E0B)
private val ZoneDanger = Color(0xFFEF4444)
private val TrackColor = Color(0xFF1E293B)
private val WatermarkTargetColor = Color(0xFF00E5FF)

/**
 * Circular Arc Speedometer-style Buffer Occupancy & Watermark Gauge.
 *
 * @param occupancyPercent Current SPSC buffer fill percentage [0.0f, 100.0f].
 * @param sampleRate Current audio sample rate in Hz.
 * @param jitterMs RFC 3550 inter-arrival jitter in milliseconds.
 */
@Composable
fun BufferOccupancyGauge(
    occupancyPercent: Float,
    sampleRate: Int,
    jitterMs: Float,
    modifier: Modifier = Modifier
) {
    val clampedPercent = occupancyPercent.coerceIn(0.0f, 100.0f)

    // Smooth animated gauge sweep
    val animatedPercent by animateFloatAsState(
        targetValue = clampedPercent,
        animationSpec = tween(durationMillis = 150, easing = FastOutSlowInEasing),
        label = "BufferGaugeAnimation"
    )

    // Dynamic zone color
    val statusColor by animateColorAsState(
        targetValue = when {
            clampedPercent < 15.0f || clampedPercent > 85.0f -> ZoneDanger
            clampedPercent < 30.0f || clampedPercent > 70.0f -> ZoneWarning
            else -> ZoneOptimal
        },
        label = "GaugeColorAnimation"
    )

    val statusText = when {
        clampedPercent < 15.0f -> "CRITICAL UNDERRUN"
        clampedPercent < 30.0f -> "DRIFT RE-SYNC LOW"
        clampedPercent <= 70.0f -> "WATERMARK LOCKED"
        clampedPercent <= 85.0f -> "DRIFT RE-SYNC HIGH"
        else -> "OVERFLOW HAZARD"
    }

    // Calculated buffer latency in milliseconds
    val bufferFrames = (clampedPercent / 100.0f) * 262144f
    val bufferLatencyMs = if (sampleRate > 0) (bufferFrames / sampleRate.toFloat()) * 1000f else 0.0f

    Card(
        modifier = modifier,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
        shape = RoundedCornerShape(12.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(
                text = "BUFFER WATERMARK & OCCUPANCY",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.1.sp
            )

            Spacer(modifier = Modifier.height(12.dp))

            // Speedometer Arc Gauge
            Box(
                modifier = Modifier.size(160.dp),
                contentAlignment = Alignment.Center
            ) {
                Canvas(modifier = Modifier.size(150.dp)) {
                    val strokeWidth = 14.dp.toPx()
                    val arcSize = size.width - strokeWidth
                    val topLeft = Offset(strokeWidth / 2f, strokeWidth / 2f)
                    val startAngle = 140f
                    val sweepRange = 260f

                    // 1. Background Arc Track
                    drawArc(
                        color = TrackColor,
                        startAngle = startAngle,
                        sweepAngle = sweepRange,
                        useCenter = false,
                        topLeft = topLeft,
                        size = Size(arcSize, arcSize),
                        style = Stroke(width = strokeWidth, cap = StrokeCap.Round)
                    )

                    // 2. Safe Watermark Zone Highlight (30% to 70%)
                    val safeStart = startAngle + (sweepRange * 0.30f)
                    val safeSweep = sweepRange * 0.40f
                    drawArc(
                        color = ZoneOptimal.copy(alpha = 0.20f),
                        startAngle = safeStart,
                        sweepAngle = safeSweep,
                        useCenter = false,
                        topLeft = topLeft,
                        size = Size(arcSize, arcSize),
                        style = Stroke(width = strokeWidth + 2.dp.toPx(), cap = StrokeCap.Butt)
                    )

                    // 3. Active Fill Arc
                    val currentSweep = (animatedPercent / 100.0f) * sweepRange
                    if (currentSweep > 1.0f) {
                        drawArc(
                            color = statusColor,
                            startAngle = startAngle,
                            sweepAngle = currentSweep,
                            useCenter = false,
                            topLeft = topLeft,
                            size = Size(arcSize, arcSize),
                            style = Stroke(width = strokeWidth, cap = StrokeCap.Round)
                        )
                    }

                    // 4. Target 50% Watermark Calibration Marker
                    val targetAngleRad = Math.toRadians((startAngle + (sweepRange * 0.50f)).toDouble())
                    val radius = arcSize / 2f
                    val center = Offset(size.width / 2f, size.height / 2f)
                    val innerR = radius - (strokeWidth / 2f) - 3.dp.toPx()
                    val outerR = radius + (strokeWidth / 2f) + 3.dp.toPx()

                    val markStart = Offset(
                        (center.x + innerR * Math.cos(targetAngleRad)).toFloat(),
                        (center.y + innerR * Math.sin(targetAngleRad)).toFloat()
                    )
                    val markEnd = Offset(
                        (center.x + outerR * Math.cos(targetAngleRad)).toFloat(),
                        (center.y + outerR * Math.sin(targetAngleRad)).toFloat()
                    )

                    drawLine(
                        color = WatermarkTargetColor,
                        start = markStart,
                        end = markEnd,
                        strokeWidth = 3.dp.toPx()
                    )
                }

                // Center Numerical Readout
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(
                        text = String.format(Locale.US, "%.1f%%", animatedPercent),
                        style = MaterialTheme.typography.headlineMedium,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace,
                        color = statusColor
                    )
                    Text(
                        text = String.format(Locale.US, "%.1f ms", bufferLatencyMs),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        fontFamily = FontFamily.Monospace
                    )
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            // State & Network Jitter Metric
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = statusText,
                    style = MaterialTheme.typography.labelSmall,
                    color = statusColor,
                    fontWeight = FontWeight.SemiBold
                )

                Text(
                    text = String.format(Locale.US, "Jitter: %.2f ms", jitterMs),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace
                )
            }
        }
    }
}
