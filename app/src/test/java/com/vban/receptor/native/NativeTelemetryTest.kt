package com.vban.receptor.native

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class NativeTelemetryTest {

    @Test
    fun testTelemetryDefaultValues() {
        val telemetry = NativeTelemetry()
        assertEquals(0, telemetry.sampleRate)
        assertEquals(0, telemetry.channelCount)
        assertEquals(0, telemetry.bitDepth)
        assertEquals(0L, telemetry.totalPacketsReceived)
        assertEquals(0L, telemetry.lostPackets)
        assertEquals(0.0f, telemetry.jitterMs, 0.001f)
        assertEquals(0.0f, telemetry.bufferOccupancyPercent, 0.001f)
        assertEquals(0.0f, telemetry.peakLevelLeft, 0.001f)
        assertEquals(0.0f, telemetry.peakLevelRight, 0.001f)
        assertEquals(0.0f, telemetry.targetWatermarkMs, 0.001f)
        assertEquals(0L, telemetry.unrecoverableDrops)
        assertEquals(0L, telemetry.glitchCount)
    }

    @Test
    fun testTelemetryCustomValues() {
        val telemetry = NativeTelemetry(
            sampleRate = 48000,
            channelCount = 2,
            bitDepth = 24,
            totalPacketsReceived = 1000L,
            lostPackets = 5L,
            jitterMs = 2.5f,
            bufferOccupancyPercent = 45.0f,
            peakLevelLeft = 0.5f,
            peakLevelRight = 0.8f,
            targetWatermarkMs = 8.0f,
            unrecoverableDrops = 2L,
            glitchCount = 1L
        )

        assertEquals(48000, telemetry.sampleRate)
        assertEquals(2, telemetry.channelCount)
        assertEquals(24, telemetry.bitDepth)
        assertEquals(1000L, telemetry.totalPacketsReceived)
        assertEquals(5L, telemetry.lostPackets)
        assertEquals(2.5f, telemetry.jitterMs, 0.001f)
        assertEquals(45.0f, telemetry.bufferOccupancyPercent, 0.001f)
        assertEquals(0.5f, telemetry.peakLevelLeft, 0.001f)
        assertEquals(0.8f, telemetry.peakLevelRight, 0.001f)
        assertEquals(8.0f, telemetry.targetWatermarkMs, 0.001f)
        assertEquals(2L, telemetry.unrecoverableDrops)
        assertEquals(1L, telemetry.glitchCount)
    }

    @Test
    fun testPacketLossRateCalculation() {
        val noPackets = NativeTelemetry(totalPacketsReceived = 0L, lostPackets = 0L)
        assertEquals(0.0f, noPackets.packetLossRatePercent, 0.001f)

        val withLoss = NativeTelemetry(totalPacketsReceived = 95L, lostPackets = 5L)
        // 5 / (95 + 5) * 100 = 5.0%
        assertEquals(5.0f, withLoss.packetLossRatePercent, 0.001f)
    }

    @Test
    fun testDbfsConversions() {
        assertEquals(-60.0f, NativeTelemetry.linearToDbfs(0.0f), 0.001f)
        assertEquals(-60.0f, NativeTelemetry.linearToDbfs(0.00001f), 0.001f)
        assertEquals(0.0f, NativeTelemetry.linearToDbfs(1.0f), 0.001f)
        assertEquals(0.0f, NativeTelemetry.linearToDbfs(2.0f), 0.001f)

        // 0.1 linear ~ -20 dBFS
        assertEquals(-20.0f, NativeTelemetry.linearToDbfs(0.1f), 0.1f)

        val telem = NativeTelemetry(peakLevelLeft = 1.0f, peakLevelRight = 0.0f)
        assertEquals(0.0f, telem.peakLevelLeftDbfs, 0.001f)
        assertEquals(-60.0f, telem.peakLevelRightDbfs, 0.001f)
    }
}
