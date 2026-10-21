package com.vban.receptor.network

/**
 * State machine managing zero-config one-shot auto-connect for discovered VBAN streams.
 *
 * Guarantees that:
 * 1. Auto-connect triggers only once per app session when exactly one stream is discovered.
 * 2. If the user explicitly halts playback ("DETENER RECEPCIÓN"), auto-connect latches
 *    to stopped and will NOT re-trigger even if the stream continues transmitting on the LAN.
 */
class AutoConnectManager {

    var hasAutoConnectedThisSession: Boolean = false
        private set

    var userRequestedStop: Boolean = false
        private set

    /**
     * Evaluates whether auto-connection should be performed.
     *
     * @param discoveredStreams Current active streams from discovery.
     * @param isRunning True if native audio engine is currently running/playing.
     * @return The single [DiscoveredStream] to connect to, or null if conditions are not met.
     */
    fun shouldAutoConnect(
        discoveredStreams: List<DiscoveredStream>,
        isRunning: Boolean
    ): DiscoveredStream? {
        if (!isRunning && !hasAutoConnectedThisSession && !userRequestedStop && discoveredStreams.size == 1) {
            return discoveredStreams[0]
        }
        return null
    }

    /**
     * Marks that the auto-connect action has been dispatched for this session.
     */
    fun markAutoConnected() {
        hasAutoConnectedThisSession = true
    }

    /**
     * Records that the user manually requested to stop audio reception.
     * Prevents subsequent auto-reconnect loops.
     */
    fun recordUserStop() {
        userRequestedStop = true
    }

    /**
     * Resets session flags (e.g., for testing or if a fresh session is started).
     */
    fun reset() {
        hasAutoConnectedThisSession = false
        userRequestedStop = false
    }
}
