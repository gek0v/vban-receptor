package com.vban.receptor.ui.theme

import androidx.compose.ui.graphics.Color

// ============================================================================
// Minimalist Bauhaus Palette (OLED Pure Black & High Contrast)
// ============================================================================
val OledBlack = Color(0xFF000000)
val DarkSurface = Color(0xFF0A0A0A)
val DarkSurfaceVariant = Color(0xFF141414)
val DarkBorder = Color(0xFF262626)

// Functional Status Accents
val LiveGreen = Color(0xFF34C759)
val StopRed = Color(0xFFFF3B30)

// Minimalist High-Contrast Typography Grays
val TextPrimary = Color(0xFFFFFFFF)
val TextMuted = Color(0xFF737373)
val TextSecondary = Color(0xFFA3A3A3)

// ============================================================================
// Compatibility Aliases for components transitioning to Bauhaus design
// ============================================================================
val DarkBackground = OledBlack
val SurfaceDark = DarkSurface
val SurfaceVariantDark = DarkSurfaceVariant
val OutlineDark = DarkBorder
val OnSurfaceLight = TextPrimary
val OnSurfaceMuted = TextMuted

val CyanAccent = Color(0xFF00E5FF)
val GoldAccent = Color(0xFFFFD600)
val EmeraldGreen = LiveGreen
val CrimsonRed = StopRed
val WarningAmber = Color(0xFFF59E0B)
