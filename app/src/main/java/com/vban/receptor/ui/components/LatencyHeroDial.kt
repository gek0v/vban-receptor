package com.vban.receptor.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.ui.theme.DarkBorder
import com.vban.receptor.ui.theme.DarkSurface
import com.vban.receptor.ui.theme.HeroDigits
import com.vban.receptor.ui.theme.HeroLabel
import com.vban.receptor.ui.theme.HeroUnits
import com.vban.receptor.ui.theme.LiveGreen
import com.vban.receptor.ui.theme.TextMuted
import com.vban.receptor.ui.theme.TextPrimary
import com.vban.receptor.ui.theme.WarningAmber
import java.util.Locale

/**
 * Health status readout for latency and network telemetry.
 */
data class LatencyHealth(
    val caption: String,
    val color: Color
)

/**
 * Computes health caption and indicator color based on network telemetry and receiver state.
 *
 * - When not running: "STANDBY · Esperando señal" (TextMuted)
 * - When running and jitterMs > 4.0 ms: "▲ Wi-Fi Jitter ($jitterMs ms) · $drops drops" (WarningAmber)
 * - When running and drops > 0: "▲ Pérdidas detectadas · $drops drops / $glitches glitches" (WarningAmber)
 * - When running and healthy: "● Óptimo · $drops drops / $glitches glitches" (LiveGreen)
 */
fun computeLatencyHealth(
    isRunning: Boolean,
    jitterMs: Float,
    drops: Long,
    glitches: Long
): LatencyHealth {
    if (!isRunning) {
        return LatencyHealth(
            caption = "STANDBY · Esperando señal",
            color = TextMuted
        )
    }
    if (jitterMs > 4.0f) {
        val formattedJitter = String.format(Locale.US, "%.1f", jitterMs)
        return LatencyHealth(
            caption = "▲ Wi-Fi Jitter ($formattedJitter ms) · $drops drops",
            color = WarningAmber
        )
    }
    if (drops > 0L) {
        return LatencyHealth(
            caption = "▲ Pérdidas detectadas · $drops drops / $glitches glitches",
            color = WarningAmber
        )
    }
    return LatencyHealth(
        caption = "● Óptimo · $drops drops / $glitches glitches",
        color = LiveGreen
    )
}

/**
 * Formats numerical latency display string.
 * Returns formatted 1-decimal string (e.g. "7.2") when running, or "--.-" when in standby.
 */
fun formatLatencyValue(latencyMs: Float, isRunning: Boolean): String {
    return if (isRunning) {
        String.format(Locale.US, "%.1f", latencyMs)
    } else {
        "--.-"
    }
}

/**
 * High-contrast Bauhaus Hero Latency Dial.
 *
 * Displays giant 48sp numerical latency with "ms" unit and dynamic health status caption
 * indicating optimal network conditions, Wi-Fi jitter, packet drops, or standby.
 *
 * @param latencyMs Current buffer latency in milliseconds.
 * @param jitterMs Current RFC 3550 inter-arrival jitter in milliseconds.
 * @param drops Count of unrecoverable packet losses.
 * @param glitches Count of audio underruns recorded.
 * @param isRunning True if audio engine is active; false renders standby state.
 * @param modifier Optional [Modifier].
 */
@Composable
fun LatencyHeroDial(
    latencyMs: Float,
    jitterMs: Float,
    drops: Long,
    glitches: Long,
    isRunning: Boolean,
    modifier: Modifier = Modifier
) {
    val health = computeLatencyHealth(
        isRunning = isRunning,
        jitterMs = jitterMs,
        drops = drops,
        glitches = glitches
    )
    val latencyText = formatLatencyValue(latencyMs, isRunning)

    Card(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = DarkSurface),
        border = BorderStroke(1.dp, DarkBorder)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp, vertical = 18.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            // Section Header Label
            Text(
                text = "LATENCIA ESTIMADA",
                style = HeroLabel,
                color = TextMuted,
                letterSpacing = 1.5.sp
            )

            // Giant Numerical Display with baseline-aligned Unit
            Row(
                verticalAlignment = Alignment.Bottom,
                horizontalArrangement = Arrangement.Start
            ) {
                Text(
                    text = latencyText,
                    style = HeroDigits,
                    color = if (isRunning) TextPrimary else TextMuted
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = "ms",
                    style = HeroUnits,
                    color = TextMuted,
                    modifier = Modifier.padding(bottom = 6.dp)
                )
            }

            // Health Status Caption
            Text(
                text = health.caption,
                style = MaterialTheme.typography.bodySmall.copy(
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Medium
                ),
                color = health.color
            )
        }
    }
}
