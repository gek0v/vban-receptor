package com.vban.receptor.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.unit.sp
import com.vban.receptor.ui.theme.DarkBorder
import com.vban.receptor.ui.theme.DarkSurface
import com.vban.receptor.ui.theme.DarkSurfaceVariant
import com.vban.receptor.ui.theme.LiveGreen
import com.vban.receptor.ui.theme.OledBlack
import com.vban.receptor.ui.theme.StopRed
import com.vban.receptor.ui.theme.TextMuted
import com.vban.receptor.ui.theme.TextPrimary
import com.vban.receptor.ui.theme.TextSecondary
import com.vban.receptor.ui.theme.BauhausDarkColorScheme
import com.vban.receptor.ui.theme.VbanTypography
import org.junit.Assert.assertEquals
import org.junit.Test

class ThemeTest {
    @Test
    fun testPaletteColors() {
        assertEquals(Color(0xFF000000), OledBlack)
        assertEquals(0xFF000000.toInt(), OledBlack.toArgb())

        assertEquals(Color(0xFF0A0A0A), DarkSurface)
        assertEquals(0xFF0A0A0A.toInt(), DarkSurface.toArgb())

        assertEquals(Color(0xFF141414), DarkSurfaceVariant)
        assertEquals(0xFF141414.toInt(), DarkSurfaceVariant.toArgb())

        assertEquals(Color(0xFF262626), DarkBorder)
        assertEquals(0xFF262626.toInt(), DarkBorder.toArgb())

        assertEquals(Color(0xFF34C759), LiveGreen)
        assertEquals(0xFF34C759.toInt(), LiveGreen.toArgb())

        assertEquals(Color(0xFFFF3B30), StopRed)
        assertEquals(0xFFFF3B30.toInt(), StopRed.toArgb())

        assertEquals(Color(0xFFFFFFFF), TextPrimary)
        assertEquals(0xFFFFFFFF.toInt(), TextPrimary.toArgb())

        assertEquals(Color(0xFF737373), TextMuted)
        assertEquals(0xFF737373.toInt(), TextMuted.toArgb())

        assertEquals(Color(0xFFA3A3A3), TextSecondary)
        assertEquals(0xFFA3A3A3.toInt(), TextSecondary.toArgb())
    }

    @Test
    fun testDarkColorSchemeMapping() {
        assertEquals(OledBlack, BauhausDarkColorScheme.background)
        assertEquals(DarkSurface, BauhausDarkColorScheme.surface)
        assertEquals(DarkSurfaceVariant, BauhausDarkColorScheme.surfaceVariant)
        assertEquals(DarkBorder, BauhausDarkColorScheme.outline)
        assertEquals(TextPrimary, BauhausDarkColorScheme.onBackground)
        assertEquals(TextPrimary, BauhausDarkColorScheme.onSurface)
        assertEquals(TextSecondary, BauhausDarkColorScheme.onSurfaceVariant)
        assertEquals(StopRed, BauhausDarkColorScheme.error)
    }

    @Test
    fun testTypographyScale() {
        assertEquals(48.sp, VbanTypography.displayLarge.fontSize)
        assertEquals(18.sp, VbanTypography.titleLarge.fontSize)
        assertEquals(11.sp, VbanTypography.labelSmall.fontSize)
    }
}
