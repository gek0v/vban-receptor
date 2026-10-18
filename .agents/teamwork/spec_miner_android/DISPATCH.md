# Dispatch Log

## 2026-10-18T14:11:18Z
**Sender**: 9bb0c473-fb0a-499a-bbf2-a430786d0058
**Role**: teamwork_preview_spec_miner (Android Audio, NDK, Oboe, Android 14+ platform APIs)
**Content**:
You are a teamwork_preview_spec_miner specializing in Android Audio, NDK, Oboe, and Android 14+ platform APIs.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\spec_miner_android
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUT:
Read the user request at: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md

OBJECTIVE:
Investigate and document the exact Android platform specifications, APIs, and constraints required:
1. Oboe 1.9.0 C++ Audio Engine:
   - Exclusive mode low latency configuration (`AudioStreamBuilder`, `SharingMode::Exclusive`, `PerformanceMode::LowLatency`).
   - Callback mechanism (`AudioStreamDataCallback`, `AudioStreamErrorCallback`).
   - Handling sample rates up to 768 kHz with AAudio backend.
   - Stream restart and recovery on audio device disconnection or sample rate change.
2. Android 14+ (API 34) Bit-Perfect Mixer Bypass for USB DACs:
   - Exact Android 14 API for bit-perfect audio (`AudioMixerAttributes`, `MIXER_BEHAVIOR_BIT_PERFECT`, `AudioManager.setPreferredMixerAttributes`, `AudioTrack` or native AAudio equivalent).
   - How to detect USB audio devices supporting bit-perfect output.
   - Backward compatibility / fallback for Android 13 and below.
3. Android Foreground Service & Power/Network Locks:
   - Foreground service declaration with type `mediaPlayback` in AndroidManifest.xml (`android:foregroundServiceType="mediaPlayback"`).
   - Android 13+ (API 33) notification permission (`POST_NOTIFICATIONS`) and foreground service permissions (`FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_MEDIA_PLAYBACK`).
   - Power & Network locks:
     - `WifiManager.WifiLock` with `WIFI_MODE_FULL_LOW_LATENCY` (API 29+).
     - `PowerManager.WakeLock` (`PARTIAL_WAKE_LOCK`).
     - `WifiManager.MulticastLock` for UDP multicast reception.
4. Jetpack Compose UI & Telemetry Requirements:
   - Reactive state management for connection controls (Start/Stop, IP, Port, Stream Name).
   - Audio device picker (built-in speaker, headphones, USB DAC).
   - Real-time stereo VU meters (Peak/RMS, decibel scale).
   - Jitter / buffer occupancy gauge (percentage or ms fill level).
   - Detected stream telemetry (sample rate, bit depth, channel count, packet loss counter).

OUTPUT REQUIREMENTS:
- Write your comprehensive specification findings to D:\Workspaces\vban-receptor\.agents\teamwork\spec_miner_android\survey_report.md
- Write your standard handoff report to D:\Workspaces\vban-receptor\.agents\teamwork\spec_miner_android\handoff.md
- Update progress.md in your directory.
- Send a message back to your parent orchestrator (Recipient: 9bb0c473-fb0a-499a-bbf2-a430786d0058) reporting completion and key findings.
Do NOT modify any source code outside your directory.
