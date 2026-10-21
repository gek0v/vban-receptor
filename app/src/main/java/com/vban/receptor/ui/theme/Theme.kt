package com.vban.receptor.ui.theme

import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

/**
 * Minimalist Bauhaus OLED Dark Color Scheme.
 * Pure black #000000 background for maximum contrast and OLED battery efficiency,
 * stark white and gray typography, subtle borders, and precise functional accents.
 */
val BauhausDarkColorScheme: ColorScheme = darkColorScheme(
    primary = TextPrimary,
    onPrimary = OledBlack,
    secondary = LiveGreen,
    onSecondary = OledBlack,
    tertiary = StopRed,
    onTertiary = TextPrimary,
    background = OledBlack,
    onBackground = TextPrimary,
    surface = DarkSurface,
    onSurface = TextPrimary,
    surfaceVariant = DarkSurfaceVariant,
    onSurfaceVariant = TextSecondary,
    outline = DarkBorder,
    outlineVariant = DarkBorder,
    error = StopRed,
    onError = TextPrimary
)

// Legacy alias
val AudiophileColorScheme: ColorScheme = BauhausDarkColorScheme

@Composable
fun VbanReceptorTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = BauhausDarkColorScheme,
        typography = VbanTypography,
        content = content
    )
}
