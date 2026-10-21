package com.vban.receptor.ui.components

import com.vban.receptor.ui.theme.LiveGreen
import com.vban.receptor.ui.theme.TextMuted
import com.vban.receptor.ui.theme.WarningAmber
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class MinimalComponentsTest {

    // ========================================================================
    // LatencyHeroDial Logic Tests
    // ========================================================================

    @Test
    fun testLatencyHealth_whenNotRunning_returnsStandby() {
        val health = computeLatencyHealth(
            isRunning = false,
            jitterMs = 0f,
            drops = 0L,
            glitches = 0L
        )
        assertEquals("STANDBY · Esperando señal", health.caption)
        assertEquals(TextMuted, health.color)
    }

    @Test
    fun testLatencyHealth_whenRunningAndOptimal_returnsOptimalGreen() {
        val health = computeLatencyHealth(
            isRunning = true,
            jitterMs = 1.8f,
            drops = 0L,
            glitches = 0L
        )
        assertEquals("● Óptimo · 0 drops / 0 glitches", health.caption)
        assertEquals(LiveGreen, health.color)
    }

    @Test
    fun testLatencyHealth_whenRunningAtJitterThreshold_returnsOptimalGreen() {
        val health = computeLatencyHealth(
            isRunning = true,
            jitterMs = 4.0f,
            drops = 0L,
            glitches = 1L
        )
        assertEquals("● Óptimo · 0 drops / 1 glitches", health.caption)
        assertEquals(LiveGreen, health.color)
    }

    @Test
    fun testLatencyHealth_whenJitterExceedsThreshold_returnsJitterAmber() {
        val health = computeLatencyHealth(
            isRunning = true,
            jitterMs = 5.2f,
            drops = 0L,
            glitches = 0L
        )
        assertEquals("▲ Wi-Fi Jitter (5.2 ms) · 0 drops", health.caption)
        assertEquals(WarningAmber, health.color)
    }

    @Test
    fun testLatencyHealth_whenDropsPresent_returnsWarningAmber() {
        val health = computeLatencyHealth(
            isRunning = true,
            jitterMs = 2.0f,
            drops = 5L,
            glitches = 1L
        )
        assertEquals("▲ Pérdidas detectadas · 5 drops / 1 glitches", health.caption)
        assertEquals(WarningAmber, health.color)
    }

    @Test
    fun testFormatLatencyValue() {
        assertEquals("7.2", formatLatencyValue(7.24f, isRunning = true))
        assertEquals("12.0", formatLatencyValue(12.0f, isRunning = true))
        assertEquals("0.0", formatLatencyValue(0.0f, isRunning = true))
        assertEquals("--.-", formatLatencyValue(7.2f, isRunning = false))
        assertEquals("--.-", formatLatencyValue(0.0f, isRunning = false))
    }

    // ========================================================================
    // MinimalStereoMeter Logic Tests
    // ========================================================================

    @Test
    fun testLinearToDbfs() {
        // Full scale 1.0f is 0 dBFS
        assertEquals(0.0f, MinimalMeterUtils.linearToDbfs(1.0f), 0.01f)

        // Half scale ~0.5f is ~ -6.02 dBFS
        assertEquals(-6.02f, MinimalMeterUtils.linearToDbfs(0.5f), 0.05f)

        // Silence / zero / negative clamps to -60.0 dB
        assertEquals(-60.0f, MinimalMeterUtils.linearToDbfs(0.0f), 0.01f)
        assertEquals(-60.0f, MinimalMeterUtils.linearToDbfs(-0.5f), 0.01f)
        assertEquals(-60.0f, MinimalMeterUtils.linearToDbfs(0.000001f), 0.01f)
    }

    @Test
    fun testDbfsToNormalized() {
        assertEquals(1.0f, MinimalMeterUtils.dbfsToNormalized(0.0f), 0.001f)
        assertEquals(0.0f, MinimalMeterUtils.dbfsToNormalized(-60.0f), 0.001f)
        assertEquals(0.5f, MinimalMeterUtils.dbfsToNormalized(-30.0f), 0.001f)

        // Clamping out-of-range bounds
        assertEquals(0.0f, MinimalMeterUtils.dbfsToNormalized(-100.0f), 0.001f)
        assertEquals(1.0f, MinimalMeterUtils.dbfsToNormalized(6.0f), 0.001f)
    }

    @Test
    fun testFormatStereoMeterText() {
        assertEquals("L -3.1 dB · R -3.5 dB", formatStereoMeterText(-3.1f, -3.5f))
        assertEquals("L 0.0 dB · R -60.0 dB", formatStereoMeterText(0.0f, -60.0f))
    }

    @Test
    fun testFormatStereoDbfs() {
        assertEquals("L -3.1 dB · R -3.5 dB", formatStereoDbfs(-3.1f, -3.5f, isRunning = true))
        assertEquals("L -∞ dB · R -∞ dB", formatStereoDbfs(-3.1f, -3.5f, isRunning = false))
    }

    // ========================================================================
    // CollapsibleDrawer Tests
    // ========================================================================

    @Test
    fun testCollapsibleDrawerDefaults() {
        assertEquals("HERRAMIENTAS AVANZADAS", DEFAULT_DRAWER_TITLE)
    }

    @Test
    fun testCollapsibleDrawerToggleLogic() {
        var isExpanded = false
        val toggle = { isExpanded = !isExpanded }

        assertFalse(isExpanded)
        toggle()
        assertTrue(isExpanded)
        toggle()
        assertFalse(isExpanded)
    }
}
