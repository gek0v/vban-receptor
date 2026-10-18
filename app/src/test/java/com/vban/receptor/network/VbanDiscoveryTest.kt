package com.vban.receptor.network

import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

@OptIn(ExperimentalCoroutinesApi::class)
class VbanDiscoveryTest {

    /**
     * Helper to synthesize a valid or custom 28-byte VBAN header packet.
     */
    private fun createVbanHeader(
        magic: Int = VbanDiscoveryManager.VBAN_MAGIC,
        subProtocol: Int = 0x00, // 0x00 = Audio
        srIndex: Int = 3,         // 3 = 48000 Hz
        nbs: Int = 63,           // 64 sample frames per packet
        nbc: Int = 1,            // 2 channels (Stereo)
        codec: Int = 0x00,       // PCM
        dataType: Int = 0x01,    // 0x01 = Int16
        streamName: String = "Stream1",
        nuFrame: Long = 100L
    ): ByteArray {
        val buffer = ByteBuffer.allocate(28).order(ByteOrder.LITTLE_ENDIAN)
        // 0..3: Magic FourCC 'V', 'B', 'A', 'N' (0x4E414256 LE)
        buffer.putInt(magic)

        // 4: format_SR (Bits 7..5 = Subprotocol, Bits 4..0 = SR index)
        val formatSr = ((subProtocol and 0x07) shl 5) or (srIndex and 0x1F)
        buffer.put(formatSr.toByte())

        // 5: format_nbs (samples per frame - 1)
        buffer.put((nbs and 0xFF).toByte())

        // 6: format_nbc (channels - 1)
        buffer.put((nbc and 0xFF).toByte())

        // 7: format_bit (Bits 7..4 = Codec, Bits 2..0 = DataType)
        val formatBit = ((codec and 0x0F) shl 4) or (dataType and 0x07)
        buffer.put(formatBit.toByte())

        // 8..23: StreamName (16 bytes ASCII, null padded)
        val nameBytes = streamName.toByteArray(Charsets.US_ASCII)
        val namePadded = ByteArray(16)
        System.arraycopy(nameBytes, 0, namePadded, 0, minOf(nameBytes.size, 16))
        buffer.put(namePadded)

        // 24..27: nuFrame (uint32 Little-Endian)
        buffer.putInt(nuFrame.toInt())

        return buffer.array()
    }

    // =========================================================================
    // 1. Header Parsing Tests
    // =========================================================================

    @Test
    fun testParseValidVbanAudioHeader48kHzStereo16Bit() {
        val packet = createVbanHeader(
            srIndex = 3,         // 48000 Hz
            nbc = 1,            // 2 channels (Stereo)
            dataType = 0x01,    // Int16
            streamName = "Stream1"
        )

        val stream = VbanDiscoveryManager.parseHeader(
            packetData = packet,
            sourceIp = "192.168.1.100",
            port = 6980,
            timestampMs = 12345678L
        )

        assertNotNull(stream)
        assertEquals("Stream1", stream!!.streamName)
        assertEquals("192.168.1.100", stream.sourceIp)
        assertEquals(6980, stream.port)
        assertEquals(48000, stream.sampleRate)
        assertEquals(2, stream.channelCount)
        assertEquals(16, stream.bitDepth)
        assertEquals(12345678L, stream.lastSeenMs)
        assertEquals("48 kHz | 16-bit | Stereo", stream.formatBadge)
    }

    @Test
    fun testParseValidVbanAudioHeader96kHzStereo24Bit() {
        val packet = createVbanHeader(
            srIndex = 4,         // 96000 Hz
            nbc = 1,            // 2 channels
            dataType = 0x02,    // Int24
            streamName = "Voicemeeter"
        )

        val stream = VbanDiscoveryManager.parseHeader(
            packetData = packet,
            sourceIp = "192.168.1.55",
            port = 6980
        )

        assertNotNull(stream)
        assertEquals("Voicemeeter", stream!!.streamName)
        assertEquals(96000, stream.sampleRate)
        assertEquals(2, stream.channelCount)
        assertEquals(24, stream.bitDepth)
        assertEquals("96 kHz | 24-bit | Stereo", stream.formatBadge)
    }

    @Test
    fun testParseValidVbanAudioHeader44k1MonoFloat32() {
        val packet = createVbanHeader(
            srIndex = 16,        // 44100 Hz
            nbc = 0,            // 1 channel (Mono)
            dataType = 0x04,    // Float32 (32-bit)
            streamName = "MicMaster"
        )

        val stream = VbanDiscoveryManager.parseHeader(
            packetData = packet,
            sourceIp = "10.0.0.12",
            port = 6980
        )

        assertNotNull(stream)
        assertEquals("MicMaster", stream!!.streamName)
        assertEquals(44100, stream.sampleRate)
        assertEquals(1, stream.channelCount)
        assertEquals(32, stream.bitDepth)
        assertEquals("44.1 kHz | 32-bit | Mono", stream.formatBadge)
    }

    @Test
    fun testParseExtended768kHzAudiophileRate() {
        val packet = createVbanHeader(
            srIndex = 21,        // 768000 Hz (audiophile extension)
            nbc = 1,            // Stereo
            dataType = 0x03,    // Int32
            streamName = "UltraHD"
        )

        val stream = VbanDiscoveryManager.parseHeader(
            packetData = packet,
            sourceIp = "192.168.1.200",
            port = 6980
        )

        assertNotNull(stream)
        assertEquals("UltraHD", stream!!.streamName)
        assertEquals(768000, stream.sampleRate)
        assertEquals(32, stream.bitDepth)
        assertEquals("768 kHz | 32-bit | Stereo", stream.formatBadge)
    }

    @Test
    fun testParseAll21OfficialSampleRates() {
        val expectedRates = intArrayOf(
            6000, 12000, 24000, 48000, 96000, 192000, 384000,
            8000, 16000, 32000, 64000, 128000, 256000, 512000,
            11025, 22050, 44100, 88200, 176400, 352800, 705600
        )

        for (idx in expectedRates.indices) {
            val packet = createVbanHeader(srIndex = idx)
            val stream = VbanDiscoveryManager.parseHeader(packet)
            assertNotNull("Rate index $idx should be valid", stream)
            assertEquals("Sample rate for index $idx mismatch", expectedRates[idx], stream!!.sampleRate)
        }
    }

    @Test
    fun testRejectShortPacket() {
        val shortPacket = ByteArray(27) // Less than 28 bytes
        val stream = VbanDiscoveryManager.parseHeader(shortPacket)
        assertNull("Packet shorter than 28 bytes must be rejected", stream)
    }

    @Test
    fun testRejectInvalidMagic() {
        val packet = createVbanHeader(magic = 0x12345678)
        val stream = VbanDiscoveryManager.parseHeader(packet)
        assertNull("Packet with invalid magic must be rejected", stream)
    }

    @Test
    fun testRejectNonAudioSubprotocol() {
        // Serial subprotocol = 0x20
        val packet = createVbanHeader(subProtocol = 1)
        val stream = VbanDiscoveryManager.parseHeader(packet)
        assertNull("Non-audio VBAN packets should not be treated as audio streams", stream)
    }

    @Test
    fun testRejectOutOfRangeSampleRateIndex() {
        // Index 25 is beyond the 22-entry rate table
        val packet = createVbanHeader(srIndex = 25)
        val stream = VbanDiscoveryManager.parseHeader(packet)
        assertNull("Out-of-range sample rate index must be rejected", stream)
    }

    @Test
    fun testStreamNameWithSpacesAndNullPadded() {
        val packet = createVbanHeader(streamName = "Room Audio  ")
        val stream = VbanDiscoveryManager.parseHeader(packet)
        assertNotNull(stream)
        assertEquals("Room Audio", stream!!.streamName)
    }

    // =========================================================================
    // 2. Stream Caching & Multi-Stream Tests
    // =========================================================================

    @Test
    fun testStreamCachingAndDeduplication() = runTest {
        val testDispatcher = StandardTestDispatcher(testScheduler)
        val testScope = TestScope(testDispatcher)
        val manager = VbanDiscoveryManager(
            evictionTimeoutMs = 4000L,
            ioDispatcher = testDispatcher,
            externalScope = testScope
        )

        val packet1 = createVbanHeader(streamName = "MainBus")
        val packet2 = createVbanHeader(streamName = "AuxBus")

        // Ingest packet from transmitter A
        manager.processPacket(packet1, packet1.size, "192.168.1.10", 6980, 1000L)
        assertEquals(1, manager.discoveredStreams.value.size)
        assertEquals("MainBus", manager.discoveredStreams.value[0].streamName)
        assertEquals(1000L, manager.discoveredStreams.value[0].lastSeenMs)

        // Ingest packet from transmitter B
        manager.processPacket(packet2, packet2.size, "192.168.1.20", 6980, 1200L)
        assertEquals(2, manager.discoveredStreams.value.size)

        // Refresh transmitter A with a newer timestamp
        manager.processPacket(packet1, packet1.size, "192.168.1.10", 6980, 2000L)
        assertEquals("Stream count should still be 2 after refresh", 2, manager.discoveredStreams.value.size)

        val updatedStreamA = manager.discoveredStreams.value.first { it.sourceIp == "192.168.1.10" }
        assertEquals(2000L, updatedStreamA.lastSeenMs)

        manager.clear()
        assertEquals(0, manager.discoveredStreams.value.size)
    }

    // =========================================================================
    // 3. Timeout Expiration (Eviction) Tests
    // =========================================================================

    @Test
    fun testStreamEvictionAfterTimeout() = runTest {
        val testDispatcher = StandardTestDispatcher(testScheduler)
        val testScope = TestScope(testDispatcher)
        val manager = VbanDiscoveryManager(
            evictionTimeoutMs = 4000L,
            ioDispatcher = testDispatcher,
            externalScope = testScope
        )

        val packetA = createVbanHeader(streamName = "StreamA")
        val packetB = createVbanHeader(streamName = "StreamB")

        // Stream A seen at t = 1000ms
        manager.processPacket(packetA, packetA.size, "192.168.1.10", 6980, 1000L)
        // Stream B seen at t = 3000ms
        manager.processPacket(packetB, packetB.size, "192.168.1.20", 6980, 3000L)
        assertEquals(2, manager.discoveredStreams.value.size)

        // At t = 4500ms:
        // Stream A age: 3500ms (<= 4000ms) -> should NOT be evicted
        // Stream B age: 1500ms (<= 4000ms) -> should NOT be evicted
        val evictedAt4500 = manager.evictExpiredStreams(4500L)
        assertFalse("No stream should be evicted at 4500ms", evictedAt4500)
        assertEquals(2, manager.discoveredStreams.value.size)

        // At t = 5500ms:
        // Stream A age: 4500ms (> 4000ms) -> MUST be evicted!
        // Stream B age: 2500ms (<= 4000ms) -> RETAINED
        val evictedAt5500 = manager.evictExpiredStreams(5500L)
        assertTrue("Stream A should be evicted at 5500ms", evictedAt5500)
        assertEquals(1, manager.discoveredStreams.value.size)
        assertEquals("StreamB", manager.discoveredStreams.value[0].streamName)

        // At t = 7500ms:
        // Stream B age: 4500ms (> 4000ms) -> MUST be evicted!
        val evictedAt7500 = manager.evictExpiredStreams(7500L)
        assertTrue("Stream B should be evicted at 7500ms", evictedAt7500)
        assertTrue("All streams should be evicted", manager.discoveredStreams.value.isEmpty())
    }
}
