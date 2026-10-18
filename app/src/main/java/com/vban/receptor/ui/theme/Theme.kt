package com.vban.receptor.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val AudiophileColorScheme = darkColorScheme(
    primary = CyanAccent,
    secondary = GoldAccent,
    background = DarkBackground,
    surface = SurfaceDark,
    surfaceVariant = SurfaceVariantDark,
    outline = OutlineDark,
    onBackground = OnSurfaceLight,
    onSurface = OnSurfaceLight,
    onSurfaceVariant = OnSurfaceMuted
)

@Composable
fun VbanReceptorTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = AudiophileColorScheme,
        typography = VbanTypography,
        content = content
    )
}
