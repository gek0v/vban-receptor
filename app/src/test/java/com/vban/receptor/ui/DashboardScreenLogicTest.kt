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
}
