# DISPATCH — Milestone 5 Service & Power Management Explorer

## Objective
Investigate and design production-ready Kotlin code templates for the Android 14+ Foreground Service and 3-Lock Manager:
1. `VbanPlaybackService.kt`:
   - Android 14 (API 34) `android:foregroundServiceType="mediaPlayback"`.
   - Notification channel creation with `IMPORTANCE_LOW` (silent ongoing playback notification).
   - Dynamic persistent notification displaying live stream status, sample rate, bit depth, packet loss %, and a Stop action button.
   - Binder interface / Service actions (`ACTION_START`, `ACTION_STOP`, `EXTRA_LISTEN_IP`, `EXTRA_PORT`, `EXTRA_STREAM_NAME`, `EXTRA_DEVICE_ID`).
2. 3-Lock Manager:
   - `WifiManager.WifiLock` with `WIFI_MODE_FULL_LOW_LATENCY` (to prevent Android Wi-Fi power-save packet bundling and latency spikes).
   - `PowerManager.WakeLock` with `PARTIAL_WAKE_LOCK` (to prevent CPU throttling during background screen-off playback).
   - `WifiManager.MulticastLock` (`createMulticastLock("vban_multicast_lock")` and `setReferenceCounted(false)`) to ensure multicast datagrams pass kernel filters on physical Wi-Fi hardware.
   - Safe acquire and release lifecycle management in service `onStartCommand` and `onDestroy`.
3. Integration with `NativeVbanEngine.kt`.
4. Output: Complete architectural survey and production-grade Kotlin class templates in `survey_report.md` and `handoff.md`.


## 2026-10-18T17:27:07Z
You are an Explorer agent for Milestone 5: Android Service & 3-Lock Power Management Architecture.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_service
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Scope Document: D:\Workspaces\vban-receptor\PROJECT.md § Interface Contracts & Milestones (M5)
- Dispatch File: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_service\DISPATCH.md
- Existing Kotlin Engine: D:\Workspaces\vban-receptor\app\src\main\java\com\vban\receptor\native\NativeVbanEngine.kt
- Android Manifest: D:\Workspaces\vban-receptor\app\src\main\AndroidManifest.xml

OBJECTIVE:
Investigate and design production-ready Kotlin code templates for the Android 14+ Foreground Service and 3-Lock Power Manager:
1. `VbanPlaybackService.kt`:
   - Android 14 (API 34) `mediaPlayback` foreground service declaration and implementation.
   - Ongoing persistent notification with low-importance channel, live telemetry updates, and pending intent controls (Start/Stop).
   - Clean service lifecycle (startForeground, stopForeground, stopSelf, binder interface).
2. 3-Lock Manager:
   - `WifiLock` (`WIFI_MODE_FULL_LOW_LATENCY`)
   - `WakeLock` (`PARTIAL_WAKE_LOCK`)
   - `MulticastLock` (`createMulticastLock("vban_multicast")`, non-ref-counted)
   - Proper acquisition in service start and guaranteed release in service destroy / error.
3. AndroidManifest.xml updates:
   - Required permissions: `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_MEDIA_PLAYBACK`, `WAKE_LOCK`, `CHANGE_WIFI_MULTICAST_STATE`, `POST_NOTIFICATIONS`.
   - Service declaration with `android:foregroundServiceType="mediaPlayback"`.
4. Output:
   - Write comprehensive survey findings and production Kotlin class templates to D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_service\survey_report.md.
   - Write handoff report to D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m5_service\handoff.md.
   - Update progress.md and notify parent orchestrator.
Do NOT modify any source code files directly outside your directory.
