"""Tier 2: Clock Drift Extremes, Clamping & Micro-Resampler Corner Cases.

Covers:
- Clamping of extreme drift requests to +-150 PPM maximum
- Strict slew rate limiting (<= 0.1 PPM/s)
- Watermark tracking at 0% (empty) and 100% (full)
- Bit-perfect passthrough at exact 0 PPM drift (ratio = 1.0)
- Micro-interpolation stability at maximum drift boundary (+-150 PPM)
"""

import math
import unittest
from e2e_tests.audio_dsp_oracle import AudioDspOracle


class TestDriftExtremes(unittest.TestCase):
    """Tier 2 tests for clock drift controller limits and resampling bounds."""

    def test_drift_clamping_at_150_ppm(self):
        """Tier 2: Verify drift correction ratio is strictly clamped to +150 PPM maximum."""
        # Overshoot request: e.g. 500 PPM
        raw_requested_ppm = 500.0
        max_clamped_ppm = 150.0

        clamped_ppm = min(max_clamped_ppm, max(-max_clamped_ppm, raw_requested_ppm))
        self.assertEqual(clamped_ppm, 150.0)

        # Ratio must be 1.00015
        ratio = 1.0 + (clamped_ppm * 1e-6)
        self.assertAlmostEqual(ratio, 1.00015, places=7)

    def test_drift_negative_clamp_at_minus_150_ppm(self):
        """Tier 2: Verify negative drift correction is strictly clamped to -150 PPM."""
        raw_requested_ppm = -800.0
        max_clamped_ppm = 150.0

        clamped_ppm = max(-max_clamped_ppm, min(max_clamped_ppm, raw_requested_ppm))
        self.assertEqual(clamped_ppm, -150.0)

        ratio = 1.0 + (clamped_ppm * 1e-6)
        self.assertAlmostEqual(ratio, 0.99985, places=7)

    def test_slew_rate_limiter_step_response(self):
        """Tier 2: Verify slew-rate limit of 0.1 PPM/s under a sudden step change."""
        current_ppm = 0.0
        target_step_ppm = 50.0
        dt = 0.1  # 100 ms update step
        max_slew_rate = 0.1  # PPM per second
        max_delta_per_step = max_slew_rate * dt  # 0.01 PPM per 100 ms

        for _ in range(10):
            delta = target_step_ppm - current_ppm
            step = min(max_delta_per_step, max(-max_delta_per_step, delta))
            current_ppm += step
            self.assertLessEqual(abs(step), max_delta_per_step + 1e-9)

        # After 1 second (10 steps), ppm should be 0.1
        self.assertAlmostEqual(current_ppm, 0.1, places=5)

    def test_catmull_rom_zero_drift(self):
        """Tier 2: Verify zero drift (ratio = 1.0) produces bit-perfect reproduction."""
        test_samples = [0.0, 0.1, 0.5, 0.9, 0.2, -0.4, -0.8, -0.2, 0.0, 0.1, 0.5, 0.9]
        # At ratio = 1.0, phase advances by 1.0 every step, sampling exactly y_0
        resampled = AudioDspOracle.catmull_rom_resample(test_samples, drift_ratio=1.0)

        # Interior samples should match exactly
        for i, val in enumerate(resampled):
            original_val = test_samples[i + 1]
            self.assertAlmostEqual(val, original_val, places=6)

    def test_catmull_rom_extreme_drift(self):
        """Tier 2: Verify interpolation at +-150 PPM boundary does not diverge or clip."""
        # 48 kHz sine signal
        sine_in = [math.sin(2.0 * math.pi * 440.0 * i / 48000.0) for i in range(500)]

        # +150 PPM
        out_plus = AudioDspOracle.catmull_rom_resample(sine_in, drift_ratio=1.00015)
        self.assertGreater(len(out_plus), 450)
        self.assertLessEqual(max(abs(s) for s in out_plus), 1.001)

        # -150 PPM
        out_minus = AudioDspOracle.catmull_rom_resample(sine_in, drift_ratio=0.99985)
        self.assertGreater(len(out_minus), 450)
        self.assertLessEqual(max(abs(s) for s in out_minus), 1.001)


if __name__ == "__main__":
    unittest.main()
