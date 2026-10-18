# DISPATCH — Milestone 5 Worker (Android Service, Bit-Perfect DAC & Jetpack Compose UI)

## Objective
Implement the complete Android framework, service, bit-perfect audio device control, and Jetpack Compose UI layers:
1. `app/src/main/AndroidManifest.xml`:
   - Add permissions: `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_MEDIA_PLAYBACK`, `WAKE_LOCK`, `CHANGE_WIFI_MULTICAST_STATE`, `POST_NOTIFICATIONS`, `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`.
   - Declare `VbanPlaybackService` with `android:foregroundServiceType="mediaPlayback"` and `android:exported="false"`.
   - Declare `MainActivity` as the exported launcher activity with intent filter `MAIN` / `LAUNCHER`.
2. `app/src/main/java/com/vban/receptor/service/PowerNetworkLockManager.kt`:
   - 3-Lock Manager acquiring and releasing `WifiLock` (`WIFI_MODE_FULL_LOW_LATENCY`), `WakeLock` (`PARTIAL_WAKE_LOCK`), and `MulticastLock` (`vban_multicast`, non-ref-counted).
3. `app/src/main/java/com/vban/receptor/service/VbanPlaybackService.kt`:
   - Android 14+ `mediaPlayback` Foreground Service.
   - Low-importance notification channel with persistent status notification (showing sample rate, bit depth, loss %, and Stop action button).
   - Dynamic Intent action handling (`ACTION_START`, `ACTION_STOP`, `ACTION_SET_DEVICE`).
   - Integration with `NativeVbanEngine` and `PowerNetworkLockManager`.
4. `app/src/main/java/com/vban/receptor/audio/AudioDeviceManager.kt`:
   - Output device discovery via `AudioManager.getDevices(GET_DEVICES_OUTPUTS)`, filtering USB DACs (`TYPE_USB_DEVICE`, `TYPE_USB_HEADSET`).
   - Hotplug listening via `AudioDeviceCallback`.
5. `app/src/main/java/com/vban/receptor/audio/BitPerfectController.kt`:
   - Android 14+ `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT` query and setup on target USB DAC.
   - Graceful fallback for API < 34 and unsupported devices.
   - Clean mixer attribute release in `clearPreferredMixerAttributes`.
6. Audiophile UI Theme (`app/src/main/java/com/vban/receptor/ui/theme/`):
   - `Color.kt`, `Type.kt`, `Theme.kt`: Dark audiophile Material 3 design system.
7. Audiophile UI Components (`app/src/main/java/com/vban/receptor/ui/components/`):
   - `StereoVuMeter.kt`: 60 FPS Compose Canvas stereo meter with DIN 45406 / EBU R128 ballistics, peak-hold decay, and clipping indicator.
   - `BufferOccupancyGauge.kt`: Arc speedometer gauge with 30%-70% safe zone and watermark indicator.
   - `DeviceSelector.kt`: Device dropdown with bit-perfect indicator badge.
8. Dashboard Screen & Activity:
   - `app/src/main/java/com/vban/receptor/ui/DashboardScreen.kt`: Full audiophile deck with connection card, audio device card, VU meter, buffer gauge, and telemetry grid.
   - `app/src/main/java/com/vban/receptor/MainActivity.kt`: Edge-to-edge Compose activity with `POST_NOTIFICATIONS` launcher and service bindings.
9. Verification:
   - Build native test runner: `cmake --build native_tests/build` and run `.\native_tests\build\vban_native_tests.exe` (96/96 pass).
   - Build Android debug APK: `.\gradlew.bat assembleDebug` (must compile cleanly and package debug APK).
   - Run E2E test suite: `python e2e_tests/run_tests.py` (84/84 pass).

## 2026-10-18T17:34:50Z
You are the Implementation Worker for Milestone 5: Android Service, Bit-Perfect DAC & Jetpack Compose UI for VBAN Receptor.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\worker_m5
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Scope Document: D:\Workspaces\vban-receptor\PROJECT.md § Interface Contracts & Milestones (M5)
- Service Explorer Survey & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_service\survey_report.md
- Bit-Perfect DAC Explorer Survey & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_bitperfect\survey_report.md
- Compose UI Explorer Survey & Templates: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_ui\survey_report.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

EXCLUSIVE WRITE OWNERSHIP (You own ONLY these files, do NOT touch other files):
- app/src/main/AndroidManifest.xml
- app/src/main/java/com/vban/receptor/service/PowerNetworkLockManager.kt
- app/src/main/java/com/vban/receptor/service/VbanPlaybackService.kt
- app/src/main/java/com/vban/receptor/audio/AudioDeviceManager.kt
- app/src/main/java/com/vban/receptor/audio/BitPerfectController.kt
- app/src/main/java/com/vban/receptor/ui/theme/Color.kt
- app/src/main/java/com/vban/receptor/ui/theme/Type.kt
- app/src/main/java/com/vban/receptor/ui/theme/Theme.kt
- app/src/main/java/com/vban/receptor/ui/components/StereoVuMeter.kt
- app/src/main/java/com/vban/receptor/ui/components/BufferOccupancyGauge.kt
- app/src/main/java/com/vban/receptor/ui/components/DeviceSelector.kt
- app/src/main/java/com/vban/receptor/ui/DashboardScreen.kt
- app/src/main/java/com/vban/receptor/MainActivity.kt
