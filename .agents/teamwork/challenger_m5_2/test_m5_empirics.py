import math
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

def run_tests():
    print("=" * 70)
    print("CHALLENGER 2 EMPIRICAL TEST HARNESS - MILESTONE 5")
    print("=" * 70)
    
    total_tests = 0
    passed_tests = 0

    def assert_eq(actual, expected, msg, tol=1e-4):
        nonlocal total_tests, passed_tests
        total_tests += 1
        if isinstance(expected, float):
            match = abs(actual - expected) <= tol
        else:
            match = actual == expected
        if match:
            passed_tests += 1
            print(f"  [PASS] {msg} -> {actual}")
        else:
            print(f"  [FAIL] {msg} -> Expected {expected}, got {actual}")
            raise AssertionError(f"Assertion failed: {msg} (got {actual}, expected {expected})")

    # -------------------------------------------------------------
    # Test Suite 1: VU Meter Linear to dBFS conversion & formatting
    # -------------------------------------------------------------
    print("\n--- Test Suite 1: VU Meter Math & Formatting ---")
    def linear_to_dbfs(linear: float) -> float:
        if math.isnan(linear) or linear <= 0.00001:
            return -60.0
        db = 20.0 * math.log10(linear)
        return max(-60.0, min(0.0, db))

    def format_dbfs(db: float) -> str:
        if db <= -59.5:
            return "-∞ dB"
        return f"{db:5.1f}"

    assert_eq(linear_to_dbfs(1.0), 0.0, "0 dBFS peak fullscale (1.0)")
    assert_eq(linear_to_dbfs(0.5), -6.0206, "-6 dBFS half amplitude (0.5)")
    assert_eq(linear_to_dbfs(0.1), -20.0, "-20 dBFS reference level (0.1)")
    assert_eq(linear_to_dbfs(0.001), -60.0, "-60 dBFS noise floor (0.001)")
    assert_eq(linear_to_dbfs(0.000001), -60.0, "Below threshold returns -60.0 dBFS")
    assert_eq(linear_to_dbfs(0.0), -60.0, "Zero linear returns -60.0 dBFS")
    assert_eq(linear_to_dbfs(-0.5), -60.0, "Negative amplitude returns -60.0 dBFS")
    assert_eq(linear_to_dbfs(2.0), 0.0, "Over-unity amplitude clamps to 0.0 dBFS")
    assert_eq(linear_to_dbfs(float('nan')), -60.0, "NaN input handled safely")

    assert_eq(format_dbfs(0.0), "  0.0", "Format 0.0 dBFS")
    assert_eq(format_dbfs(-6.02), " -6.0", "Format -6.0 dBFS")
    assert_eq(format_dbfs(-20.0), "-20.0", "Format -20.0 dBFS")
    assert_eq(format_dbfs(-59.5), "-∞ dB", "Format -59.5 dBFS cutoff to -∞")
    assert_eq(format_dbfs(-60.0), "-∞ dB", "Format -60.0 dBFS to -∞")

    # -------------------------------------------------------------
    # Test Suite 2: DIN 45406 / EBU R128 Ballistics Simulation
    # -------------------------------------------------------------
    print("\n--- Test Suite 2: 60 FPS VU Ballistics Simulation ---")
    class VuBallisticsSim:
        def __init__(self):
            self.current_db = -60.0
            self.peak_hold_db = -60.0
            self.peak_hold_timer_ms = 0
            self.clip_timer_ms = 0

        def update(self, linear_in: float, dt_sec: float, is_running: bool):
            target_db = linear_to_dbfs(linear_in) if is_running else -60.0
            dt_ms = int(dt_sec * 1000.0)

            release_decay = 20.0 * dt_sec
            peak_decay = 15.0 * dt_sec

            # Instant attack or release decay
            if target_db >= self.current_db:
                self.current_db = target_db
            else:
                self.current_db = max(target_db, self.current_db - release_decay)

            # Peak hold logic
            if self.current_db >= self.peak_hold_db:
                self.peak_hold_db = self.current_db
                self.peak_hold_timer_ms = 1200
            elif self.peak_hold_timer_ms > 0:
                self.peak_hold_timer_ms = max(0, self.peak_hold_timer_ms - dt_ms)
            else:
                self.peak_hold_db = max(-60.0, self.peak_hold_db - peak_decay)

            # Clip detection
            if linear_in >= 0.999 and is_running:
                self.clip_timer_ms = 1000
            elif self.clip_timer_ms > 0:
                self.clip_timer_ms = max(0, self.clip_timer_ms - dt_ms)

    sim = VuBallisticsSim()
    dt = 1.0 / 60.0 # 16.666 ms per frame

    # 1. Instant attack: 0 dBFS input
    sim.update(1.0, dt, True)
    assert_eq(sim.current_db, 0.0, "Instant attack reaches 0 dBFS in 1 frame")
    assert_eq(sim.peak_hold_db, 0.0, "Peak hold catches 0 dBFS in 1 frame")
    assert_eq(sim.peak_hold_timer_ms, 1200, "Peak hold timer set to 1200 ms")
    assert_eq(sim.clip_timer_ms, 1000, "Clip LED timer set to 1000 ms")

    # 2. Input drops to silence; simulate 60 frames (1.0 second)
    for _ in range(60):
        sim.update(0.0, dt, True)
    # After 1 sec, current_db drops by 20 dB/s * 1s = 20 dB -> -20 dBFS
    assert_eq(sim.current_db, -20.0, "20 dB/s release decay after 1.0s drops to -20 dBFS", tol=0.1)
    # Peak hold should still be pegged at 0 dBFS because timer was 1200 ms and only 960 ms passed
    assert_eq(sim.peak_hold_db, 0.0, "Peak hold remains at 0 dBFS during 1200 ms hold window")
    assert_eq(sim.clip_timer_ms, 40, "Clip timer has 40 ms left after 60 frames")

    # 3. Simulate 3 more frames (48 ms) -> clip timer expires
    for _ in range(3):
        sim.update(0.0, dt, True)
    assert_eq(sim.clip_timer_ms, 0, "Clip timer expired after 63 frames (~1.05s)")

    # 4. Simulate another 15 frames (240 ms) -> peak hold timer expires (1248 ms total)
    for _ in range(15):
        sim.update(0.0, dt, True)
    assert_eq(sim.peak_hold_timer_ms, 0, "Peak hold timer is now 0")
    # Peak hold has started decaying at 15 dB/s
    if sim.peak_hold_db < 0.0:
        passed_tests += 1
        total_tests += 1
        print(f"  [PASS] Peak hold is now decaying: {sim.peak_hold_db:.2f} dBFS")
    else:
        raise AssertionError("Peak hold did not start decaying after timer expiry")

    # 5. Peak decay at 15 dB/s: from -0.75 dBFS to -60 dBFS takes ~237 more frames (~3.95s)
    for _ in range(240):
        sim.update(0.0, dt, True)
    assert_eq(sim.current_db, -60.0, "Level meter settled at -60 dBFS noise floor")
    assert_eq(sim.peak_hold_db, -60.0, "Peak hold settled at -60 dBFS noise floor after 4.0s decay")

    # -------------------------------------------------------------
    # Test Suite 3: Buffer Occupancy & Speedometer Latency Math
    # -------------------------------------------------------------
    print("\n--- Test Suite 3: Buffer Occupancy & Latency Math ---")
    def calc_buffer_latency(occupancy_pct: float, sample_rate: int) -> float:
        clamped_pct = max(0.0, min(100.0, occupancy_pct))
        buffer_frames = (clamped_pct / 100.0) * 262144.0
        return (buffer_frames / float(sample_rate)) * 1000.0 if sample_rate > 0 else 0.0

    def get_zone_status(occupancy_pct: float) -> str:
        clamped = max(0.0, min(100.0, occupancy_pct))
        if clamped < 15.0:
            return "CRITICAL UNDERRUN"
        elif clamped < 30.0:
            return "DRIFT RE-SYNC LOW"
        elif clamped <= 70.0:
            return "WATERMARK LOCKED"
        elif clamped <= 85.0:
            return "DRIFT RE-SYNC HIGH"
        else:
            return "OVERFLOW HAZARD"

    # Latency verification
    # At 768 kHz, full buffer (262144 frames)
    lat_768_full = calc_buffer_latency(100.0, 768000)
    assert_eq(lat_768_full, 341.3333, "768 kHz 100% buffer latency == 341.33 ms", tol=0.01)

    # At 48 kHz, 50% target watermark (131072 frames)
    lat_48_target = calc_buffer_latency(50.0, 48000)
    assert_eq(lat_48_target, 2730.6667, "48 kHz 50% watermark latency == 2730.67 ms", tol=0.01)

    # Edge cases
    assert_eq(calc_buffer_latency(50.0, 0), 0.0, "0 Hz sample rate yields 0.0 ms without crashing")
    assert_eq(calc_buffer_latency(-20.0, 48000), 0.0, "Negative occupancy clamps to 0.0 ms")
    assert_eq(calc_buffer_latency(150.0, 48000), calc_buffer_latency(100.0, 48000), "Over 100% occupancy clamps to 100%")

    # Zone Status Classification
    assert_eq(get_zone_status(0.0), "CRITICAL UNDERRUN", "0% is CRITICAL UNDERRUN")
    assert_eq(get_zone_status(14.9), "CRITICAL UNDERRUN", "14.9% is CRITICAL UNDERRUN")
    assert_eq(get_zone_status(15.0), "DRIFT RE-SYNC LOW", "15.0% is DRIFT RE-SYNC LOW")
    assert_eq(get_zone_status(29.9), "DRIFT RE-SYNC LOW", "29.9% is DRIFT RE-SYNC LOW")
    assert_eq(get_zone_status(30.0), "WATERMARK LOCKED", "30.0% is WATERMARK LOCKED")
    assert_eq(get_zone_status(50.0), "WATERMARK LOCKED", "50.0% is WATERMARK LOCKED")
    assert_eq(get_zone_status(70.0), "WATERMARK LOCKED", "70.0% is WATERMARK LOCKED")
    assert_eq(get_zone_status(70.1), "DRIFT RE-SYNC HIGH", "70.1% is DRIFT RE-SYNC HIGH")
    assert_eq(get_zone_status(85.0), "DRIFT RE-SYNC HIGH", "85.0% is DRIFT RE-SYNC HIGH")
    assert_eq(get_zone_status(85.1), "OVERFLOW HAZARD", "85.1% is OVERFLOW HAZARD")
    assert_eq(get_zone_status(100.0), "OVERFLOW HAZARD", "100.0% is OVERFLOW HAZARD")

    # -------------------------------------------------------------
    # Test Suite 4: Bit-Perfect Decision Matrix & State Machine
    # -------------------------------------------------------------
    print("\n--- Test Suite 4: Bit-Perfect Decision Matrix ---")
    TYPE_BUILTIN_SPEAKER = 2
    TYPE_WIRED_HEADPHONES = 3
    TYPE_USB_DEVICE = 11
    TYPE_USB_HEADSET = 22
    TYPE_USB_ACCESSORY = 12

    MIXER_BEHAVIOR_DEFAULT = 0
    MIXER_BEHAVIOR_BIT_PERFECT = 1

    def evaluate_bit_perfect(api_version: int, dev_type: int, hal_behaviors: list[int]):
        if api_version < 34:
            return ("FALLBACK", "OS_VERSION_BELOW_34")
        if dev_type not in (TYPE_USB_DEVICE, TYPE_USB_HEADSET, TYPE_USB_ACCESSORY):
            return ("FALLBACK", "DEVICE_NOT_USB")
        if MIXER_BEHAVIOR_BIT_PERFECT not in hal_behaviors:
            return ("FALLBACK", "HAL_DOES_NOT_SUPPORT_BIT_PERFECT")
        return ("ACTIVE", None)

    assert_eq(evaluate_bit_perfect(33, TYPE_USB_DEVICE, [MIXER_BEHAVIOR_BIT_PERFECT]),
              ("FALLBACK", "OS_VERSION_BELOW_34"), "Android 13 falls back for USB DAC")
    assert_eq(evaluate_bit_perfect(34, TYPE_BUILTIN_SPEAKER, [MIXER_BEHAVIOR_BIT_PERFECT]),
              ("FALLBACK", "DEVICE_NOT_USB"), "Android 14 speaker falls back")
    assert_eq(evaluate_bit_perfect(34, TYPE_WIRED_HEADPHONES, [MIXER_BEHAVIOR_BIT_PERFECT]),
              ("FALLBACK", "DEVICE_NOT_USB"), "Android 14 3.5mm headphones falls back")
    assert_eq(evaluate_bit_perfect(34, TYPE_USB_DEVICE, [MIXER_BEHAVIOR_DEFAULT]),
              ("FALLBACK", "HAL_DOES_NOT_SUPPORT_BIT_PERFECT"), "Android 14 non-bit-perfect USB DAC falls back")
    assert_eq(evaluate_bit_perfect(34, TYPE_USB_DEVICE, [MIXER_BEHAVIOR_BIT_PERFECT]),
              ("ACTIVE", None), "Android 14 Bit-Perfect USB DAC engages ACTIVE")
    assert_eq(evaluate_bit_perfect(34, TYPE_USB_HEADSET, [MIXER_BEHAVIOR_BIT_PERFECT]),
              ("ACTIVE", None), "Android 14 Bit-Perfect USB Headset engages ACTIVE")

    print(f"\nALL {passed_tests}/{total_tests} EMPIRICAL UNIT TESTS PASSED!")
    return total_tests, passed_tests

if __name__ == "__main__":
    run_tests()
