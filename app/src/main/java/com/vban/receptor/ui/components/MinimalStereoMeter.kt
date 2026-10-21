package com.vban.receptor.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.ui.theme.DarkBorder
import com.vban.receptor.ui.theme.DarkSurface
import com.vban.receptor.ui.theme.HeroLabel
import com.vban.receptor.ui.theme.StopRed
import com.vban.receptor.ui.theme.TextMuted
import com.vban.receptor.ui.theme.TextPrimary
import com.vban.receptor.ui.theme.TextSecondary
import kotlinx.coroutines.isActive
import java.util.Locale

/**
 * Utility functions and formatting helpers for minimal stereo level meters.
 */
object MinimalMeterUtils {
    const val MIN_METER_DB = -60.0f

    /**
     * Converts a linear amplitude [0.0, 1.0+] to dBFS clamped to [-60.0, 0.0].
     */
    fun linearToDbfs(linear: Float): Float {
        if (linear <= 0.00001f) return MIN_METER_DB
        val db = 20.0f * kotlin.math.log10(linear)
        return db.coerceIn(MIN_METER_DB, 0.0f)
    }

    /**
     * Normalizes dBFS in [minDb, 0.0] to a unit fraction [0.0, 1.0].
     */
    fun dbfsToNormalized(dbfs: Float, minDb: Float = MIN_METER_DB): Float {
        if (dbfs <= minDb) return 0.0f
        if (dbfs >= 0.0f) return 1.0f
        return ((dbfs - minDb) / (0.0f - minDb)).coerceIn(0.0f, 1.0f)
    }

    /**
     * Formats dBFS values into standard stereo readout: "L -3.1 dB · R -3.5 dB".
     */
    fun formatStereoMeterText(leftDbfs: Float, rightDbfs: Float): String {
        return "L ${String.format(Locale.US, "%.1f", leftDbfs)} dB · R ${String.format(Locale.US, "%.1f", rightDbfs)} dB"
    }

    /**
     * Formats dBFS values with standby awareness.
     */
    fun formatStereoDbfs(leftDbfs: Float, rightDbfs: Float, isRunning: Boolean = true): String {
        return if (isRunning) {
            formatStereoMeterText(leftDbfs, rightDbfs)
        } else {
            "L -∞ dB · R -∞ dB"
        }
    }
}

// Top-level aliases for convenience
fun formatStereoMeterText(leftDbfs: Float, rightDbfs: Float): String =
    MinimalMeterUtils.formatStereoMeterText(leftDbfs, rightDbfs)

fun formatStereoDbfs(leftDbfs: Float, rightDbfs: Float, isRunning: Boolean = true): String =
    MinimalMeterUtils.formatStereoDbfs(leftDbfs, rightDbfs, isRunning)

/**
 * Minimalist Stereo Level Meter card.
 *
 * Renders dual-line horizontal micro-bars (6dp height each) for Left and Right channels,
 * with fast-attack smooth exponential decay ballistics and numerical dBFS readout.
 *
 * @param leftLinear Linear peak amplitude for Left channel [0.0, 1.0].
 * @param rightLinear Linear peak amplitude for Right channel [0.0, 1.0].
 * @param isRunning True if engine is streaming; false renders idle standby state.
 * @param modifier Optional [Modifier].
 */
@Composable
fun MinimalStereoMeter(
    leftLinear: Float,
    rightLinear: Float,
    isRunning: Boolean,
    modifier: Modifier = Modifier
) {
    var currentLeftDb by remember { mutableFloatStateOf(MinimalMeterUtils.MIN_METER_DB) }
    var currentRightDb by remember { mutableFloatStateOf(MinimalMeterUtils.MIN_METER_DB) }

    val currentLeftLinear by rememberUpdatedState(leftLinear)
    val currentRightLinear by rememberUpdatedState(rightLinear)
    val currentIsRunning by rememberUpdatedState(isRunning)

    // 60 FPS Ballistic loop with fast-attack and smooth exponential decay
    LaunchedEffect(isRunning) {
        if (!isRunning) {
            currentLeftDb = MinimalMeterUtils.MIN_METER_DB
            currentRightDb = MinimalMeterUtils.MIN_METER_DB
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
                lastFrameNanos = frameNanos

                val targetLeftDb = if (currentIsRunning) MinimalMeterUtils.linearToDbfs(currentLeftLinear) else MinimalMeterUtils.MIN_METER_DB
                val targetRightDb = if (currentIsRunning) MinimalMeterUtils.linearToDbfs(currentRightLinear) else MinimalMeterUtils.MIN_METER_DB

                // Ballistic Decay: 40 dB/s decay rate
                val decay = 40.0f * dtSec

                currentLeftDb = if (targetLeftDb >= currentLeftDb) {
                    targetLeftDb // Fast attack
                } else {
                    (currentLeftDb - decay).coerceAtLeast(targetLeftDb)
                }

                currentRightDb = if (targetRightDb >= currentRightDb) {
                    targetRightDb // Fast attack
                } else {
                    (currentRightDb - decay).coerceAtLeast(targetRightDb)
                }
            }
        }
    }

    val leftFraction = MinimalMeterUtils.dbfsToNormalized(currentLeftDb)
    val rightFraction = MinimalMeterUtils.dbfsToNormalized(currentRightDb)
    val isLeftClip = currentLeftDb >= -0.5f && isRunning
    val isRightClip = currentRightDb >= -0.5f && isRunning

    val readoutText = formatStereoDbfs(currentLeftDb, currentRightDb, isRunning)

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
            // Header Row: Section Label on Left, dBFS readout on Right
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "NIVEL ESTÉREO",
                    style = HeroLabel,
                    color = TextMuted,
                    letterSpacing = 1.5.sp
                )
                Text(
                    text = readoutText,
                    style = MaterialTheme.typography.labelSmall.copy(
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.SemiBold
                    ),
                    color = if (isRunning) TextSecondary else TextMuted
                )
            }

            // Left Channel Micro-Bar
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = "L",
                    style = MaterialTheme.typography.labelSmall.copy(
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold
                    ),
                    color = TextMuted,
                    modifier = Modifier.width(12.dp)
                )
                MicroBar(
                    fraction = leftFraction,
                    isClipping = isLeftClip,
                    modifier = Modifier.weight(1f)
                )
            }

            // Right Channel Micro-Bar
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    text = "R",
                    style = MaterialTheme.typography.labelSmall.copy(
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold
                    ),
                    color = TextMuted,
                    modifier = Modifier.width(12.dp)
                )
                MicroBar(
                    fraction = rightFraction,
                    isClipping = isRightClip,
                    modifier = Modifier.weight(1f)
                )
            }
        }
    }
}

/**
 * Thin 6dp horizontal audio level bar with high-contrast Bauhaus monochrome styling.
 */
@Composable
private fun MicroBar(
    fraction: Float,
    isClipping: Boolean,
    modifier: Modifier = Modifier
) {
    Canvas(
        modifier = modifier
            .fillMaxWidth()
            .height(6.dp)
    ) {
        val cornerRadius = CornerRadius(size.height / 2f, size.height / 2f)

        // Background track
        drawRoundRect(
            color = Color(0xFF1A1A1A),
            size = size,
            cornerRadius = cornerRadius
        )

        // Active fill
        val activeWidth = (size.width * fraction.coerceIn(0f, 1f))
        if (activeWidth > 0f) {
            val barColor = if (isClipping) StopRed else TextPrimary
            drawRoundRect(
                color = barColor,
                size = Size(activeWidth, size.height),
                cornerRadius = cornerRadius
            )
        }
    }
}
