package com.vban.receptor.ui

import com.vban.receptor.network.AutoConnectManager
import com.vban.receptor.network.DiscoveredStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class DashboardScreenLogicTest {

    @Test
    fun testSubtitleTextFormatting() {
        fun formatSubtitle(isRunning: Boolean, sampleRate: Int): String {
            return if (isRunning && sampleRate > 0) {
                "FLOAT32 ${sampleRate / 1000}K"
            } else {
                "FLOAT32 48K"
            }
        }

        assertEquals("FLOAT32 48K", formatSubtitle(isRunning = false, sampleRate = 0))
        assertEquals("FLOAT32 48K", formatSubtitle(isRunning = false, sampleRate = 96000))
        assertEquals("FLOAT32 48K", formatSubtitle(isRunning = true, sampleRate = 48000))
        assertEquals("FLOAT32 96K", formatSubtitle(isRunning = true, sampleRate = 96000))
        assertEquals("FLOAT32 192K", formatSubtitle(isRunning = true, sampleRate = 192000))
    }

    @Test
    fun testAutoConnectLifecycleWithUserStop() {
        val manager = AutoConnectManager()
        val stream = DiscoveredStream(
            sourceIp = "192.168.1.50",
            port = 6980,
            streamName = "DeskMain",
            sampleRate = 48000,
            channelCount = 2,
            bitDepth = 16
        )

        // 1. Initial idle state with 1 stream -> should auto-connect
        val target = manager.shouldAutoConnect(listOf(stream), isRunning = false)
        assertNotNull(target)
        assertEquals("DeskMain", target?.streamName)

        // Dispatched
        manager.markAutoConnected()

        // 2. Already dispatched -> null
        assertNull(manager.shouldAutoConnect(listOf(stream), isRunning = false))

        // 3. User manually stops reception ("DETENER RECEPCIÓN")
        manager.recordUserStop()

        // 4. Reset auto-connect flag only (simulate new stream appearing while user has stopped)
        // should still be blocked because userRequestedStop is latched
        assertNull(manager.shouldAutoConnect(listOf(stream), isRunning = false))
    }

    @Test
    fun testActiveStreamDisplayIpResolution() {
        fun resolveDisplayIp(transmitterIp: String, listenIp: String): String {
            return if (transmitterIp.isNotBlank()) transmitterIp else listenIp
        }

        // Auto-connected from transmitter
        assertEquals("192.168.1.50", resolveDisplayIp(transmitterIp = "192.168.1.50", listenIp = "0.0.0.0"))
        // Manual unicast config
        assertEquals("0.0.0.0", resolveDisplayIp(transmitterIp = "", listenIp = "0.0.0.0"))
        // Multicast group
        assertEquals("239.255.0.1", resolveDisplayIp(transmitterIp = "", listenIp = "239.255.0.1"))
    }

    @Test
    fun testUpdatedSubtitleAndPillState() {
        fun computeSubtitle(isRunning: Boolean, totalPackets: Long, sampleRate: Int): String {
            val isWaiting = isRunning && totalPackets == 0L
            return when {
                !isRunning -> "STANDBY"
                isWaiting -> "ESPERANDO VBAN"
                sampleRate > 0 -> "FLOAT32 ${sampleRate / 1000}K"
                else -> "FLOAT32 48K"
            }
        }

        fun computePillText(isRunning: Boolean, totalPackets: Long): String {
            val isLive = isRunning && totalPackets > 0L
            val isWaiting = isRunning && totalPackets == 0L
            return when {
                isLive -> "LIVE"
                isWaiting -> "ESPERANDO"
                else -> "STANDBY"
            }
        }

        // Standby
        assertEquals("STANDBY", computeSubtitle(isRunning = false, totalPackets = 0L, sampleRate = 0))
        assertEquals("STANDBY", computePillText(isRunning = false, totalPackets = 0L))

        // Running but waiting for first packet
        assertEquals("ESPERANDO VBAN", computeSubtitle(isRunning = true, totalPackets = 0L, sampleRate = 0))
        assertEquals("ESPERANDO", computePillText(isRunning = true, totalPackets = 0L))

        // Running and receiving stream
        assertEquals("FLOAT32 48K", computeSubtitle(isRunning = true, totalPackets = 100L, sampleRate = 48000))
        assertEquals("FLOAT32 96K", computeSubtitle(isRunning = true, totalPackets = 100L, sampleRate = 96000))
        assertEquals("LIVE", computePillText(isRunning = true, totalPackets = 100L))
    }

    @Test
    fun testActiveStreamCardFormatAndSignalReadout() {
        fun computeFormatText(isRunning: Boolean, totalPackets: Long, sampleRate: Int, bitDepth: Int): String {
            val hasSignal = isRunning && totalPackets > 0L
            return when {
                !isRunning -> "En espera"
                !hasSignal -> "0 Hz · 0-bit"
                else -> "$sampleRate Hz · $bitDepth-bit"
            }
        }

        fun computeStreamName(configured: String, detected: String): String {
            return when {
                detected.isNotBlank() -> detected
                configured.isNotBlank() -> configured
                else -> "Cualquier Flujo (Auto)"
            }
        }

        // Standby
        assertEquals("En espera", computeFormatText(isRunning = false, totalPackets = 0L, sampleRate = 0, bitDepth = 0))

        // Running, 0 packets (no signal)
        assertEquals("0 Hz · 0-bit", computeFormatText(isRunning = true, totalPackets = 0L, sampleRate = 0, bitDepth = 0))

        // Running, active 24-bit stream
        assertEquals("48000 Hz · 24-bit", computeFormatText(isRunning = true, totalPackets = 50L, sampleRate = 48000, bitDepth = 24))

        // Stream name auto detection
        assertEquals("Cualquier Flujo (Auto)", computeStreamName(configured = "", detected = ""))
        assertEquals("Stream1", computeStreamName(configured = "", detected = "Stream1"))
        assertEquals("Voicemeeter", computeStreamName(configured = "MyConfig", detected = "Voicemeeter"))
    }
}
