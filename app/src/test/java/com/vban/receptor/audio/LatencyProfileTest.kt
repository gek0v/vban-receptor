package com.vban.receptor.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test

class LatencyProfileTest {

    @Test
    fun testAutoProfileIsFirstAndDefault() {
        assertEquals(LatencyProfile.AUTO, LatencyProfile.entries.first())
        assertEquals(LatencyProfile.AUTO, LatencyProfile.DEFAULT)
    }

    @Test
    fun testAutoProfileProperties() {
        val auto = LatencyProfile.AUTO
        assertEquals("AUTO", auto.id)
        assertEquals("Auto (Adaptativo)", auto.displayName)
        assertEquals("5–25 ms · RFC 3550 Servo", auto.subtitle)
        assertEquals(5.0f, auto.targetLatencyMsMin, 0.001f)
        assertEquals(25.0f, auto.targetLatencyMsMax, 0.001f)
        assertEquals(5.0f, auto.targetLatencyMs, 0.001f)
        assertEquals("5–25 ms", auto.latencyRangeString)
    }

    @Test
    fun testAllProfilesExistAndOrdered() {
        val expected = listOf(
            LatencyProfile.AUTO,
            LatencyProfile.ULTRA_LOW,
            LatencyProfile.BALANCED,
            LatencyProfile.ANTI_JITTER
        )
        assertEquals(expected, LatencyProfile.entries)
    }

    @Test
    fun testFromId() {
        assertEquals(LatencyProfile.AUTO, LatencyProfile.fromId("AUTO"))
        assertEquals(LatencyProfile.AUTO, LatencyProfile.fromId("auto"))
        assertEquals(LatencyProfile.ULTRA_LOW, LatencyProfile.fromId("ULTRA_LOW"))
        assertEquals(LatencyProfile.BALANCED, LatencyProfile.fromId("BALANCED"))
        assertEquals(LatencyProfile.ANTI_JITTER, LatencyProfile.fromId("ANTI_JITTER"))
        assertEquals(LatencyProfile.DEFAULT, LatencyProfile.fromId("UNKNOWN"))
        assertEquals(LatencyProfile.DEFAULT, LatencyProfile.fromId(null))
    }
}
