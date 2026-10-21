package com.vban.receptor.ui.components

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
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
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bolt
import androidx.compose.material.icons.filled.NetworkCheck
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.vban.receptor.audio.LatencyProfile
import com.vban.receptor.ui.theme.CyanAccent
import com.vban.receptor.ui.theme.EmeraldGreen
import com.vban.receptor.ui.theme.GoldAccent
import com.vban.receptor.ui.theme.OutlineDark
import com.vban.receptor.ui.theme.SurfaceDark
import com.vban.receptor.ui.theme.SurfaceVariantDark

/**
 * Clean Material 3 styled segmented selector card for Adaptive Latency / Jitter Buffer Profiles.
 *
 * Supports switching dynamically between [LatencyProfile.ULTRA_LOW], [LatencyProfile.BALANCED],
 * and [LatencyProfile.ANTI_JITTER], displaying nominal buffer occupancy, filter tau, and target latency.
 *
 * @param selectedProfile Currently active latency profile.
 * @param onProfileSelected Callback triggered when user selects a new profile.
 * @param enabled Whether selector interactions are enabled.
 * @param modifier Optional [Modifier].
 */
@Composable
fun LatencyProfileSelector(
    selectedProfile: LatencyProfile,
    onProfileSelected: (LatencyProfile) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
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
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Header: Title and active target indicator badge
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
                        imageVector = Icons.Default.Tune,
                        contentDescription = "Latency Settings",
                        tint = CyanAccent,
                        modifier = Modifier.size(18.dp)
                    )
                    Text(
                        text = "LATENCY / JITTER BUFFER",
                        style = MaterialTheme.typography.labelMedium.copy(
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 1.2.sp
                        ),
                        color = Color.White
                    )
                }

                // Active latency badge pill
                Surface(
                    shape = RoundedCornerShape(6.dp),
                    color = getProfileAccentColor(selectedProfile).copy(alpha = 0.15f),
                    border = BorderStroke(1.dp, getProfileAccentColor(selectedProfile).copy(alpha = 0.4f))
                ) {
                    Text(
                        text = selectedProfile.latencyRangeString,
                        modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp),
                        style = MaterialTheme.typography.labelSmall.copy(
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.Bold
                        ),
                        color = getProfileAccentColor(selectedProfile)
                    )
                }
            }

            // Material 3 Segmented Control Row
            Surface(
                modifier = Modifier.fillMaxWidth(),
                shape = RoundedCornerShape(8.dp),
                color = SurfaceVariantDark,
                border = BorderStroke(1.dp, OutlineDark)
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(3.dp),
                    horizontalArrangement = Arrangement.spacedBy(4.dp)
                ) {
                    LatencyProfile.entries.forEach { profile ->
                        val isSelected = (profile == selectedProfile)
                        LatencySegmentItem(
                            profile = profile,
                            isSelected = isSelected,
                            enabled = enabled,
                            onClick = { onProfileSelected(profile) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
            }

            // Subtitle Description & Drift PID Telemetry Details
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0xFF0D131D), RoundedCornerShape(6.dp))
                    .padding(horizontal = 10.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(3.dp)
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(6.dp)
                            .background(getProfileAccentColor(selectedProfile), CircleShape)
                    )
                    Text(
                        text = selectedProfile.subtitle,
                        style = MaterialTheme.typography.bodySmall.copy(fontWeight = FontWeight.SemiBold),
                        color = Color(0xFFE2E8F0)
                    )
                }

                Text(
                    text = "Target Latency: ${selectedProfile.latencyRangeString} (${selectedProfile.targetLatencyMs.toInt()} ms nominal)",
                    style = MaterialTheme.typography.labelSmall.copy(
                        fontFamily = FontFamily.Monospace,
                        fontSize = 10.sp
                    ),
                    color = Color(0xFF94A3B8)
                )
            }
        }
    }
}

/**
 * Individual segmented button item inside the profile row.
 */
@Composable
private fun LatencySegmentItem(
    profile: LatencyProfile,
    isSelected: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val accentColor = getProfileAccentColor(profile)
    val animatedBg by animateColorAsState(
        targetValue = if (isSelected) accentColor.copy(alpha = 0.18f) else Color.Transparent,
        animationSpec = tween(durationMillis = 200, easing = FastOutSlowInEasing),
        label = "segmentBg"
    )
    val animatedBorder by animateColorAsState(
        targetValue = if (isSelected) accentColor.copy(alpha = 0.6f) else Color.Transparent,
        animationSpec = tween(durationMillis = 200, easing = FastOutSlowInEasing),
        label = "segmentBorder"
    )
    val animatedTextColor by animateColorAsState(
        targetValue = if (isSelected) Color.White else Color(0xFF94A3B8),
        animationSpec = tween(durationMillis = 200, easing = FastOutSlowInEasing),
        label = "segmentTextColor"
    )

    val interactionSource = remember { MutableInteractionSource() }

    Box(
        modifier = modifier
            .clip(RoundedCornerShape(6.dp))
            .background(animatedBg)
            .border(BorderStroke(1.dp, animatedBorder), RoundedCornerShape(6.dp))
            .clickable(
                interactionSource = interactionSource,
                indication = null,
                enabled = enabled,
                onClick = onClick
            )
            .padding(vertical = 8.dp, horizontal = 4.dp),
        contentAlignment = Alignment.Center
    ) {
        Column(
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Icon(
                    imageVector = getProfileIcon(profile),
                    contentDescription = null,
                    tint = if (isSelected) accentColor else Color(0xFF64748B),
                    modifier = Modifier.size(13.dp)
                )
                Text(
                    text = profile.displayName,
                    style = MaterialTheme.typography.labelSmall.copy(
                        fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Medium
                    ),
                    color = animatedTextColor,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }

            Text(
                text = "${profile.targetLatencyMsMin.toInt()}–${profile.targetLatencyMsMax.toInt()} ms",
                style = MaterialTheme.typography.labelSmall.copy(
                    fontFamily = FontFamily.Monospace,
                    fontSize = 9.sp,
                    fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Normal
                ),
                color = if (isSelected) accentColor else Color(0xFF64748B)
            )
        }
    }
}

private fun getProfileAccentColor(profile: LatencyProfile): Color {
    return when (profile) {
        LatencyProfile.ULTRA_LOW -> CyanAccent
        LatencyProfile.BALANCED -> EmeraldGreen
        LatencyProfile.ANTI_JITTER -> GoldAccent
    }
}

private fun getProfileIcon(profile: LatencyProfile): ImageVector {
    return when (profile) {
        LatencyProfile.ULTRA_LOW -> Icons.Default.Bolt
        LatencyProfile.BALANCED -> Icons.Default.Tune
        LatencyProfile.ANTI_JITTER -> Icons.Default.NetworkCheck
    }
}
