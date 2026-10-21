package com.vban.receptor.network

import android.util.Log
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.Closeable
import java.io.IOException
import java.net.DatagramPacket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.MulticastSocket
import java.net.NetworkInterface
import java.net.SocketException
import java.net.SocketTimeoutException
import java.util.concurrent.ConcurrentHashMap

/**
 * Asynchronous Zero-Config VBAN stream discovery engine.
 *
 * Listens on the network for incoming VBAN UDP packets, parses the 28-byte header,
 * extracts audio format parameters and stream names, and exposes active streams
 * via reactive [StateFlow].
 *
 * Streams not refreshed within [evictionTimeoutMs] (default 4000ms / 4s) are automatically evicted.
 */
class VbanDiscoveryManager(
    val defaultPort: Int = DEFAULT_PORT,
    private val evictionTimeoutMs: Long = DEFAULT_EVICTION_TIMEOUT_MS,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
    private val externalScope: CoroutineScope? = null
) : Closeable {

    private val scope = externalScope ?: CoroutineScope(ioDispatcher + SupervisorJob())

    private val streamCache = ConcurrentHashMap<String, DiscoveredStream>()

    private val _discoveredStreams = MutableStateFlow<List<DiscoveredStream>>(emptyList())
    val discoveredStreams: StateFlow<List<DiscoveredStream>> = _discoveredStreams.asStateFlow()

    private val _isScanning = MutableStateFlow(false)
    val isScanning: StateFlow<Boolean> = _isScanning.asStateFlow()

    private var listenerJob: Job? = null
    private var evictionJob: Job? = null
    private var activeSocket: MulticastSocket? = null
    private var currentPort: Int = defaultPort

    private val lock = Any()

    /**
     * Starts listening for VBAN UDP broadcast/unicast/multicast packets on [port].
     *
     * @param port UDP port to bind (defaults to 6980).
     */
    @Synchronized
    fun start(port: Int = defaultPort) {
        currentPort = port
        if (_isScanning.value && activeSocket?.localPort == port) {
            Log.d(TAG, "Discovery already active on port $port")
            return
        }

        stop()

        _isScanning.value = true

        listenerJob = scope.launch(ioDispatcher) {
            runDiscoveryLoop(port)
        }

        evictionJob = scope.launch(ioDispatcher) {
            runEvictionLoop()
        }

        Log.i(TAG, "VBAN stream discovery started on port $port")
    }

    /**
     * Stops the discovery engine and releases network sockets.
     */
    @Synchronized
    fun stop() {
        _isScanning.value = false

        listenerJob?.cancel()
        listenerJob = null

        evictionJob?.cancel()
        evictionJob = null

        try {
            activeSocket?.close()
        } catch (e: Exception) {
            Log.w(TAG, "Error closing discovery socket: ${e.message}")
        } finally {
            activeSocket = null
        }

        Log.i(TAG, "VBAN stream discovery stopped")
    }

    /**
     * Pauses the discovery engine and cleanly closes the UDP socket to release port 6980
     * for the native audio receiver while preserving discovered stream cache.
     */
    @Synchronized
    fun pauseDiscoverySocket() {
        if (!_isScanning.value) {
            return
        }

        _isScanning.value = false

        listenerJob?.cancel()
        listenerJob = null

        evictionJob?.cancel()
        evictionJob = null

        try {
            activeSocket?.close()
        } catch (e: Exception) {
            Log.w(TAG, "Error closing discovery socket on pause: ${e.message}")
        } finally {
            activeSocket = null
        }

        Log.i(TAG, "VBAN stream discovery paused")
    }

    /**
     * Resumes the discovery engine, re-opening the UDP socket on the last configured port.
     */
    @Synchronized
    fun resumeDiscoverySocket() {
        if (_isScanning.value) {
            return
        }

        start(currentPort)
        Log.i(TAG, "VBAN stream discovery resumed on port $currentPort")
    }

    /**
     * Clears cached streams and immediately emits an empty list.
     */
    fun clear() {
        synchronized(lock) {
            streamCache.clear()
            _discoveredStreams.value = emptyList()
        }
    }

    override fun close() {
        stop()
    }

    /**
     * Processes a single packet buffer and adds/refreshes the stream in the active cache.
     *
     * @return The parsed [DiscoveredStream] if valid, or null if ignored/malformed.
     */
    fun processPacket(
        packetData: ByteArray,
        length: Int,
        sourceIp: String,
        port: Int,
        timestampMs: Long = System.currentTimeMillis()
    ): DiscoveredStream? {
        val stream = parseHeader(
            packetData = packetData,
            length = length,
            sourceIp = sourceIp,
            port = port,
            timestampMs = timestampMs
        ) ?: return null

        addOrUpdateStream(stream)
        return stream
    }

    /**
     * Adds or updates a stream entry in the cache and publishes the updated list.
     */
    fun addOrUpdateStream(stream: DiscoveredStream) {
        synchronized(lock) {
            streamCache[stream.id] = stream
            publishStreamList()
        }
    }

    /**
     * Checks all cached streams and evicts any stream whose last seen timestamp
     * is older than [evictionTimeoutMs].
     *
     * @param nowMs Current monotonic/epoch timestamp in milliseconds.
     * @return True if any stream was evicted.
     */
    fun evictExpiredStreams(nowMs: Long = System.currentTimeMillis()): Boolean {
        synchronized(lock) {
            var evictedAny = false
            val iterator = streamCache.entries.iterator()
            while (iterator.hasNext()) {
                val entry = iterator.next()
                if (nowMs - entry.value.lastSeenMs > evictionTimeoutMs) {
                    iterator.remove()
                    evictedAny = true
                }
            }

            if (evictedAny) {
                publishStreamList()
            }
            return evictedAny
        }
    }

    private fun publishStreamList() {
        _discoveredStreams.value = streamCache.values
            .sortedWith(compareBy({ it.streamName.lowercase() }, { it.sourceIp }))
    }

    private suspend fun runDiscoveryLoop(port: Int) {
        var socket: MulticastSocket? = null
        try {
            socket = MulticastSocket(null).apply {
                reuseAddress = true
                soTimeout = SOCKET_TIMEOUT_MS
                receiveBufferSize = SOCKET_BUFFER_SIZE
                bind(InetSocketAddress(port))
            }

            synchronized(this) {
                activeSocket = socket
            }

            joinMulticastGroups(socket, port)

            val buffer = ByteArray(MAX_PACKET_SIZE)
            val packet = DatagramPacket(buffer, buffer.size)

            Log.i(TAG, "Listening for VBAN packets on 0.0.0.0:$port")

            while (scope.isActive && _isScanning.value) {
                try {
                    socket.receive(packet)
                    val sourceAddress = packet.address?.hostAddress ?: "0.0.0.0"
                    processPacket(
                        packetData = packet.data,
                        length = packet.length,
                        sourceIp = sourceAddress,
                        port = packet.port,
                        timestampMs = System.currentTimeMillis()
                    )
                } catch (_: SocketTimeoutException) {
                    // Periodic timeout to yield and check isActive
                } catch (e: SocketException) {
                    if (scope.isActive && _isScanning.value) {
                        Log.w(TAG, "SocketException in discovery loop: ${e.message}")
                    }
                    break
                } catch (e: IOException) {
                    if (scope.isActive && _isScanning.value) {
                        Log.w(TAG, "IOException in discovery loop: ${e.message}")
                    }
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to bind or initialize discovery socket on port $port: ${e.message}", e)
        } finally {
            try {
                socket?.close()
            } catch (_: Exception) {}

            synchronized(this) {
                if (activeSocket === socket) {
                    activeSocket = null
                }
            }
        }
    }

    private fun joinMulticastGroups(socket: MulticastSocket, port: Int) {
        try {
            val multicastAddress = InetAddress.getByName(DEFAULT_MULTICAST_GROUP)
            val mcastSocketAddress = InetSocketAddress(multicastAddress, port)

            val interfaces = NetworkInterface.getNetworkInterfaces() ?: return
            while (interfaces.hasMoreElements()) {
                val nif = interfaces.nextElement()
                try {
                    if (nif.isUp && nif.supportsMulticast() && !nif.isLoopback) {
                        socket.joinGroup(mcastSocketAddress, nif)
                        Log.d(TAG, "Joined VBAN multicast group $DEFAULT_MULTICAST_GROUP on interface ${nif.name}")
                    }
                } catch (e: Exception) {
                    Log.d(TAG, "Could not join multicast on interface ${nif.name}: ${e.message}")
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Multicast group join skipped: ${e.message}")
        }
    }

    private suspend fun runEvictionLoop() {
        while (scope.isActive && _isScanning.value) {
            delay(EVICTION_CHECK_INTERVAL_MS)
            evictExpiredStreams(System.currentTimeMillis())
        }
    }

    companion object {
        private const val TAG = "VbanDiscoveryManager"

        const val DEFAULT_PORT = 6980
        const val DEFAULT_MULTICAST_GROUP = "239.255.0.1"
        const val DEFAULT_EVICTION_TIMEOUT_MS = 4000L
        private const val EVICTION_CHECK_INTERVAL_MS = 500L
        private const val SOCKET_TIMEOUT_MS = 1000
        private const val SOCKET_BUFFER_SIZE = 128 * 1024
        private const val MAX_PACKET_SIZE = 2048

        /**
         * 4-byte FourCC magic header 'V', 'B', 'A', 'N' in Little-Endian byte order.
         * Value: 0x4E414256
         */
        const val VBAN_MAGIC = 0x4E414256

        const val VBAN_HEADER_SIZE = 28
        const val VBAN_STREAM_NAME_MAX_LEN = 16

        /**
         * Authoritative 21-rate table defined by VB-Audio VBAN specification,
         * with 768 kHz audiophile extension at index 21.
         */
        val VBAN_SR_TABLE = intArrayOf(
            // 6 kHz base multiplier family (indices 0..6)
            6000, 12000, 24000, 48000, 96000, 192000, 384000,
            // 8 kHz base multiplier family (indices 7..13)
            8000, 16000, 32000, 64000, 128000, 256000, 512000,
            // 11.025 kHz base multiplier family (indices 14..20)
            11025, 22050, 44100, 88200, 176400, 352800, 705600,
            // 768 kHz audiophile extension (index 21)
            768000
        )

        /**
         * Parses a 28-byte VBAN header from raw UDP packet data.
         *
         * @param packetData Raw bytes received from the network.
         * @param length Number of valid bytes in [packetData].
         * @param sourceIp IP of the transmitter.
         * @param port UDP port.
         * @param timestampMs Monotonic or epoch timestamp to associate with the stream.
         * @return [DiscoveredStream] if the header is a valid VBAN audio header, null otherwise.
         */
        fun parseHeader(
            packetData: ByteArray,
            length: Int = packetData.size,
            sourceIp: String = "0.0.0.0",
            port: Int = DEFAULT_PORT,
            timestampMs: Long = System.currentTimeMillis()
        ): DiscoveredStream? {
            if (length < VBAN_HEADER_SIZE || packetData.size < VBAN_HEADER_SIZE) {
                return null
            }

            // 1. Verify VBAN Magic (Bytes 0..3: 'V', 'B', 'A', 'N' -> 0x4E414256 LE)
            val magic = (packetData[0].toInt() and 0xFF) or
                    ((packetData[1].toInt() and 0xFF) shl 8) or
                    ((packetData[2].toInt() and 0xFF) shl 16) or
                    ((packetData[3].toInt() and 0xFF) shl 24)

            if (magic != VBAN_MAGIC) {
                return null
            }

            // 2. Format_SR (Byte 4): Bits 7..5 = Subprotocol, Bits 4..0 = Sample Rate Index
            val formatSr = packetData[4].toInt() and 0xFF
            val subProtocol = formatSr and 0xE0
            if (subProtocol != 0x00) {
                // Non-audio VBAN subprotocols (Serial=0x20, Text=0x40, Service=0x60)
                return null
            }

            val srIndex = formatSr and 0x1F
            if (srIndex !in VBAN_SR_TABLE.indices) {
                return null
            }
            val sampleRate = VBAN_SR_TABLE[srIndex]

            // 3. Format_nbc (Byte 6): Channels per frame - 1 (0 -> 1 ch, 1 -> 2 ch, etc.)
            val formatNbc = packetData[6].toInt() and 0xFF
            val channelCount = formatNbc + 1

            // 4. Format_bit (Byte 7): Bits 7..4 = Codec, Bits 2..0 = DataType
            val formatBit = packetData[7].toInt() and 0xFF
            val dataType = formatBit and 0x07
            val bitDepth = when (dataType) {
                0x00 -> 8   // Byte8
                0x01 -> 16  // Int16
                0x02 -> 24  // Int24
                0x03 -> 32  // Int32
                0x04 -> 32  // Float32
                0x05 -> 64  // Float64
                0x06 -> 12  // Bits12
                0x07 -> 10  // Bits10
                else -> 16
            }

            // 5. StreamName (Bytes 8..23: 16 bytes ASCII, null-padded/trimmed)
            var nullPos = 16
            for (i in 0 until VBAN_STREAM_NAME_MAX_LEN) {
                if (packetData[8 + i] == 0.toByte()) {
                    nullPos = i
                    break
                }
            }
            val rawName = String(packetData, 8, nullPos, Charsets.US_ASCII).trim()
            val streamName = rawName.ifBlank { "Stream" }

            return DiscoveredStream(
                streamName = streamName,
                sourceIp = sourceIp,
                port = port,
                sampleRate = sampleRate,
                channelCount = channelCount,
                bitDepth = bitDepth,
                lastSeenMs = timestampMs
            )
        }
    }
}
