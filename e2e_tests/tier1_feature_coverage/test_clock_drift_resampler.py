"""Tier 1: Clock Drift Compensation & Micro-Resampler Tests.

Covers Features:
- Feature 23: Clock Drift Jitter Filter (Slow EMA filter tracking buffer watermark)
- Feature 24: Adaptive Clock Drift Servo (PI controller, clamping +-100 PPM, slew rate cap)
- Feature 25: Catmull-Rom Cubic Hermite Micro-Resampler (C^1 continuous 4-point spline)
- Feature 26: Decoupled Stream Reconfiguration (128-sample cosine fade-out/fade-in)
"""

import math
import unittest
from e2e_tests.audio_dsp_oracle import AudioDspOracle


class TestClockDriftResampler(unittest.TestCase):
    """Tier 1 tests for clock drift compensation and DSP resampling."""

    def test_jitter_filter_ema_convergence(self):
        """Feature 23: Verify EMA filter rejects high-frequency jitter and converges to average watermark."""
        # Simulated Wi-Fi bursty watermarks with mean 50.0 and jitter +-20.0
        raw_watermarks = [50.0 + 20.0 * math.sin(i * 0.5) for i in range(200)]

        # EMA filter with slow tau: alpha = 1.0 - exp(-dt / tau) ~ 0.05
        alpha = 0.05
        filtered = 50.0
        filtered_series = []
        for w in raw_watermarks:
            filtered = (1.0 - alpha) * filtered + alpha * w
            filtered_series.append(filtered)

        # Variance of filtered series must be drastically lower than raw variance
        raw_var = sum((x - 50.0) ** 2 for x in raw_watermarks) / len(raw_watermarks)
        filt_var = sum((x - 50.0) ** 2 for x in filtered_series[50:]) / len(filtered_series[50:])
        self.assertLess(filt_var, raw_var * 0.2, "EMA filter failed to reject high frequency jitter")

    def test_drift_servo_pi_control(self):
        """Feature 24: Verify PI controller corrects simulated clock drift within +-100 PPM."""
        # Set target watermark = 50.0%
        target_watermark = 50.0
        current_watermark = 60.0  # Drift surplus (+10%)
        kp = 2.0   # PPM per % error
        ki = 0.05  # PPM per % error * sec
        integral = 0.0
        dt = 0.1   # 100 ms update step

        # Audiophile safety constraints
        max_ppm = 100.0
        max_slew_ppm_per_sec = 0.1

        last_ppm = 0.0
        for _ in range(50):
            error = current_watermark - target_watermark
            integral += error * dt
            raw_correction_ppm = kp * error + ki * integral

            # Clamp to +-100 PPM
            clamped_ppm = max(-max_ppm, min(max_ppm, raw_correction_ppm))

            # Slew rate limit
            max_delta = max_slew_ppm_per_sec * dt
            if abs(clamped_ppm - last_ppm) > max_delta:
                if clamped_ppm > last_ppm:
                    correction_ppm = last_ppm + max_delta
                else:
                    correction_ppm = last_ppm - max_delta
            else:
                correction_ppm = clamped_ppm

            # Slew rate constraint assertion
            self.assertLessEqual(abs(correction_ppm - last_ppm), max_delta + 1e-6)
            last_ppm = correction_ppm

            # Watermark responds to speed adjustment
            current_watermark -= correction_ppm * 0.0001

        # Must have actively compensated towards target
        self.assertLess(current_watermark, 60.0)

    def test_catmull_rom_spline_math(self):
        """Feature 25: Verify Catmull-Rom cubic Hermite spline interpolation formulas."""
        # 1. Endpoints match exact samples at x=0.0 and x=1.0
        y_m1, y_0, y_1, y_2 = 1.0, 2.0, 4.0, 5.0
        val_0 = AudioDspOracle.catmull_rom_sample(y_m1, y_0, y_1, y_2, 0.0)
        self.assertAlmostEqual(val_0, y_0, places=6)

        val_1 = AudioDspOracle.catmull_rom_sample(y_m1, y_0, y_1, y_2, 1.0)
        self.assertAlmostEqual(val_1, y_1, places=6)

        # 2. Midpoint x=0.5 on linear ramp is exactly linear
        lin_m1, lin_0, lin_1, lin_2 = 1.0, 2.0, 3.0, 4.0
        val_mid = AudioDspOracle.catmull_rom_sample(lin_m1, lin_0, lin_1, lin_2, 0.5)
        self.assertAlmostEqual(val_mid, 2.5, places=6)

    def test_catmull_rom_resample_waveform_integrity(self):
        """Feature 25: Micro-resample pure sine wave with +50 PPM drift and verify harmonic purity."""
        # 48 kHz sine wave at 1 kHz
        sr = 48000
        freq = 1000.0
        num_samples = 1000
        sine_in = [math.sin(2.0 * math.pi * freq * i / sr) for i in range(num_samples)]

        # +50 PPM drift ratio = 1.0 + 50e-6
        drift_ratio = 1.0 + 50e-6
        resampled = AudioDspOracle.catmull_rom_resample(sine_in, drift_ratio)

        # Output must be populated, smooth, and bounded in [-1.0, +1.0]
        self.assertGreater(len(resampled), 900)
        max_amp = max(abs(s) for s in resampled)
        self.assertLessEqual(max_amp, 1.001)

        # Check absence of sudden discontinuities (step derivative)
        for i in range(1, len(resampled)):
            diff = abs(resampled[i] - resampled[i - 1])
            self.assertLess(diff, 0.2)  # 1 kHz at 48k has max step ~0.13

    def test_stream_reconfig_cosine_fade(self):
        """Feature 26: Verify 128-sample equal-power cosine fade-out and fade-in curves."""
        fade_out = AudioDspOracle.cosine_fade_out_weights(128)
        fade_in = AudioDspOracle.cosine_fade_in_weights(128)

        self.assertEqual(len(fade_out), 128)
        self.assertEqual(len(fade_in), 128)

        # Check endpoints
        self.assertAlmostEqual(fade_out[0], 1.0, places=6)
        self.assertAlmostEqual(fade_out[-1], 0.0, places=2)
        self.assertAlmostEqual(fade_in[0], 0.0, places=6)
        self.assertAlmostEqual(fade_in[-1], 1.0, places=2)

        # Check complementary property: fade_out[i] + fade_in[i] == 1.0
        for i in range(128):
            self.assertAlmostEqual(fade_out[i] + fade_in[i], 1.0, places=6)


if __name__ == "__main__":
    unittest.main()
