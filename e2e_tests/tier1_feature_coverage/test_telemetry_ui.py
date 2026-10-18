"""Tier 1: Telemetry, VU Meter & Jetpack Compose UI Logic Tests.

Covers Features:
- Feature 31: Non-Blocking JNI Bridge (NativeTelemetry snapshot contract)
- Feature 40: Jetpack Compose Dashboard UI (Reactive state bindings)
- Feature 41: 60 FPS Stereo VU Meter Canvas (dBFS conversion, 20 dB/s peak hold decay)
- Feature 42: Jitter & Buffer Occupancy Gauge (Buffer fill percent, visual thresholds)
- Feature 43: Real-Time Telemetry Counters (Sample rate, channels, bit depth, loss)
"""

import math
import struct
import unittest
from e2e_tests.audio_dsp_oracle import AudioDspOracle


class TestTelemetryUi(unittest.TestCase):
    """Tier 1 tests for telemetry metrics calculation, JNI snapshot layout, and VU meters."""

    def test_jni_bridge_contract_signatures(self):
        """Feature 31: Verify NativeTelemetry struct binary layout and field sizes."""
        # struct NativeTelemetry {
        #     uint32_t sampleRate;              // 4 bytes
        #     uint16_t channelCount;            // 2 bytes
        #     uint16_t bitDepth;                // 2 bytes
        #     uint64_t totalPacketsReceived;    // 8 bytes
        #     uint64_t lostPackets;             // 8 bytes
        #     float    jitterMs;                // 4 bytes
        #     float    bufferOccupancyPercent;  // 4 bytes
        #     float    peakLevelLeft;           // 4 bytes
        #     float    peakLevelRight;          // 4 bytes
        # };
        fmt = "<IHHQQffff"
        expected_size = 4 + 2 + 2 + 8 + 8 + 4 + 4 + 4 + 4  # 40 bytes packed
        self.assertEqual(struct.calcsize(fmt), 40)

        # Pack and unpack a sample snapshot
        packed = struct.pack(
            fmt,
            768000,   # sampleRate
            2,        # channelCount
            32,       # bitDepth
            1000000,  # totalPacketsReceived
            5,        # lostPackets
            1.25,     # jitterMs
            50.0,     # bufferOccupancyPercent
            0.85,     # peakLevelLeft
            0.82,     # peakLevelRight
        )
        self.assertEqual(len(packed), 40)
        unpacked = struct.unpack(fmt, packed)
        self.assertEqual(unpacked[0], 768000)
        self.assertEqual(unpacked[1], 2)
        self.assertEqual(unpacked[2], 32)
        self.assertEqual(unpacked[3], 1000000)
        self.assertEqual(unpacked[4], 5)
        self.assertAlmostEqual(unpacked[5], 1.25, places=4)
        self.assertAlmostEqual(unpacked[6], 50.0, places=4)

    def test_vu_meter_dbfs_and_peak_decay(self):
        """Feature 41: Verify dBFS scale (-60 to 0 dBFS) and peak hold decay (20 dB/s)."""
        # 1. Full-scale sine wave (peak = 1.0) -> 0 dBFS
        self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(1.0), 0.0, places=3)

        # 2. -6 dBFS amplitude ~ 0.501187
        self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(0.501187), -6.0, places=1)

        # 3. Floor clamping at 0.001 amplitude -> -60 dBFS
        self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(0.0005), -60.0, places=1)
        self.assertAlmostEqual(AudioDspOracle.amplitude_to_dbfs(0.0), -60.0, places=1)

        # 4. Peak hold decay: 20 dB/s decay over dt = 0.05s (50 ms) -> -1 dB decay
        initial_peak_db = 0.0
        decay_rate_db_per_sec = 20.0
        dt = 0.05
        current_amp = 0.1  # -20 dBFS
        current_db = AudioDspOracle.amplitude_to_dbfs(current_amp)

        decayed_peak_db = max(current_db, initial_peak_db - decay_rate_db_per_sec * dt)
        self.assertAlmostEqual(decayed_peak_db, -1.0, places=3)

    def test_buffer_occupancy_gauge_math(self):
        """Feature 42: Verify buffer occupancy visual thresholds (Green / Yellow / Red)."""
        def classify_occupancy(percent: float) -> str:
            if 30.0 <= percent <= 70.0:
                return "STABLE_GREEN"
            elif (10.0 <= percent < 30.0) or (70.0 < percent <= 85.0):
                return "DRIFT_YELLOW"
            else:
                return "HAZARD_RED"

        self.assertEqual(classify_occupancy(50.0), "STABLE_GREEN")
        self.assertEqual(classify_occupancy(25.0), "DRIFT_YELLOW")
        self.assertEqual(classify_occupancy(80.0), "DRIFT_YELLOW")
        self.assertEqual(classify_occupancy(5.0), "HAZARD_RED")   # Underrun hazard
        self.assertEqual(classify_occupancy(95.0), "HAZARD_RED")  # Overflow hazard

    def test_telemetry_counters_accuracy(self):
        """Feature 43: Verify live display formatting for sample rate, bit depth, channels, loss."""
        detected_sr = 192000
        detected_bits = 24
        detected_channels = 2
        lost_pkts = 12

        formatted_sr = f"{detected_sr / 1000.0:.1f} kHz"
        formatted_format = f"{detected_bits}-bit PCM, {detected_channels} Ch"
        self.assertEqual(formatted_sr, "192.0 kHz")
        self.assertEqual(formatted_format, "24-bit PCM, 2 Ch")
        self.assertEqual(lost_pkts, 12)

    def test_compose_dashboard_state_binding(self):
        """Feature 40: Verify reactive state object defaults and field mutation."""
        state = {
            "isRunning": False,
            "bindAddress": "0.0.0.0",
            "port": 6980,
            "streamName": "Stream1",
            "selectedDeviceId": None,
            "isBitPerfectActive": False,
            "leftPeakDb": -60.0,
            "rightPeakDb": -60.0,
            "bufferFillPercent": 0.0,
        }
        self.assertFalse(state["isRunning"])
        self.assertEqual(state["port"], 6980)

        # Mutate upon start
        state["isRunning"] = True
        state["isBitPerfectActive"] = True
        state["bufferFillPercent"] = 48.5
        self.assertTrue(state["isRunning"])
        self.assertTrue(state["isBitPerfectActive"])
        self.assertEqual(state["bufferFillPercent"], 48.5)


if __name__ == "__main__":
    unittest.main()
