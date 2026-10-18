# VBAN Receptor for Android: E2E Test Infrastructure & Verification Plan

## 1. Test Philosophy & Principles

The End-to-End (E2E) Test Suite for the **VBAN Receptor for Android** provides an authoritative, requirement-driven, opaque-box verification framework. The test architecture adheres to the following core tenets:

1. **Opaque-Box (Black-Box) Testing**:
   - The test suite validates observable system behaviors, input/output contracts, protocol compliance, signal fidelity, concurrency invariants, and lifecycle transitions without coupling to private internal module implementation details.
   - Inputs consist of raw network datagrams (IPv4/IPv6 UDP unicast and multicast packets), audio frame streams, peripheral state transitions (USB DAC hotplug), and system lifecycle events.
   - Outputs consist of parsed header fields, normalized Float32 audio waveforms, ring buffer watermarks, resampled audio frames, bit-perfect mixer configuration intents, telemetry metrics (RFC 3550 jitter, dBFS peak/RMS), and power/network lock states.

2. **Requirement-Driven & Traceable**:
   - Every test case is derived directly from the authoritative specifications:
     - **ORIGINAL_REQUEST.md**: User requirements R1 to R5 and acceptance criteria.
     - **PROJECT.md**: System architecture, 45-feature inventory, and module interface contracts.
     - **VB-Audio VBAN Protocol Specification (Rev 13)**: 28-byte header, FourCC magic, 21 official sample rates + 768 kHz extension, format bitmasks, sample conversions.
     - **Android NDK & Audio Specifications**: Oboe 1.9.0 AAudio exclusive mode, Android 14+ (API 34) `AudioMixerAttributes.MIXER_BEHAVIOR_BIT_PERFECT`, Foreground Service `mediaPlayback`, `WifiLock(WIFI_MODE_FULL_LOW_LATENCY)`.
     - **DSP & Mathematics**: RFC 3550 EWMA inter-arrival jitter filter, Catmull-Rom cubic Hermite spline interpolation, 128-sample equal-power cosine crossfade, and dBFS conversions.

3. **Multi-Tiered Verification**:
   - **Tier 1 — Feature Coverage**: Comprehensive happy-path and contract verification (>= 5 test cases per feature category) covering protocol, dequantizers, SPSC ring buffer, drift compensation, network transport, Android lifecycle, bit-perfect routing, and telemetry.
   - **Tier 2 — Boundary & Corner Cases**: Stressing limits (0, min, max, integer wraparound, truncated packets, trailing garbage, IEEE 754 NaN/Inf sanitization, burst drops, drift extremes $\pm 100$ PPM).
   - **Tier 3 — Cross-Feature Combinations**: Pairwise interactions between concurrent subsystems (rate changes mid-stream with cosine crossfade, packet loss bursts during drift servo adjustments, USB DAC hotplug during 768 kHz streaming, Wi-Fi address changes).
   - **Tier 4 — Real-World Application Scenarios**: Realistic workloads running thousands of frames under varying network conditions (768 kHz 32-bit float streaming, 44.1 kHz 16-bit PCM CD streaming, 192 kHz 24-bit studio master, bursty Wi-Fi jitter recovery, multi-room multicast reception).

4. **Self-Contained & Deterministic**:
   - The test suite is fully automated, runnable directly from the workspace with zero third-party dependencies (using standard Python 3.13), and cleanly reports pass/fail counts, execution timings, and non-zero exit codes on failure.

---

## 2. Complete Feature Inventory Coverage Matrix (45 Features)

All 45 features identified in `PROJECT.md` are mapped across Tiers 1–4 of the E2E test suite:

| # | Feature | Milestone | Tier 1 (Feature) | Tier 2 (Boundaries) | Tier 3 (Cross-Feature) | Tier 4 (Real-World) | Test Module |
|---|---|:---:|:---:|:---:|:---:|:---:|---|
| 1 | NDK C++20 & CMake Scaffolding | M1 | `test_build_config_contract` | `test_abi_filter_boundaries` | `test_toolchain_flags_compat` | `test_high_rate_compilation_spec` | `test_android_lifecycle_locks.py` |
| 2 | Oboe 1.9.0 Prefab Linkage | M1 | `test_oboe_prefab_contract` | `test_oboe_buffer_limits` | `test_oboe_dac_reconnect` | `test_oboe_768k_streaming` | `test_bitperfect_audio_routing.py` |
| 3 | Standalone Native Test Harness | M1 | `test_runner_execution_harness` | `test_runner_timeout_and_exit_codes` | `test_runner_multi_suite` | `test_runner_full_workload` | `run_tests.py` |
| 4 | Fixed 28-Byte Header Structure | M2 | `test_header_size_and_packing` | `test_header_truncated_under_28` | `test_header_with_rate_switch` | `test_header_streaming_fidelity` | `test_protocol_header.py` |
| 5 | FourCC Magic Validation | M2 | `test_fourcc_magic_vban_valid` | `test_fourcc_corrupted_magic` | `test_fourcc_in_stream_transition` | `test_fourcc_real_stream_continuity` | `test_protocol_header.py` |
| 6 | Subprotocol Verification | M2 | `test_subprotocol_audio_enforced` | `test_subprotocol_serial_text_reject`| `test_subprotocol_mixed_traffic` | `test_subprotocol_multicast_filter` | `test_protocol_header.py` |
| 7 | Official 21 Sample Rates Table | M2 | `test_official_21_sample_rates` | `test_sample_rate_out_of_bounds` | `test_sample_rate_switch_sequence` | `test_cd_audio_44k_stream` | `test_protocol_header.py` |
| 8 | 768 kHz Sample Rate Extension | M2 | `test_768k_sample_rate_extension` | `test_768k_max_throughput_limits` | `test_768k_with_dac_unplug` | `test_audiophile_768k_scenario` | `test_protocol_header.py` |
| 9 | Frame Size (`format_nbs`) Decoding | M2 | `test_format_nbs_decoding` | `test_format_nbs_extremes_0_and_255` | `test_format_nbs_dynamic_change` | `test_format_nbs_real_bursts` | `test_protocol_header.py` |
| 10 | Channel Count (`format_nbc`) Decoding| M2 | `test_format_nbc_decoding` | `test_format_nbc_extremes_0_and_255` | `test_format_nbc_channel_switch` | `test_format_nbc_surround_stream` | `test_protocol_header.py` |
| 11 | Codec & Reserved Bit 3 Check | M2 | `test_codec_pcm_and_bit3_zero` | `test_bit3_set_rejection` | `test_codec_compressed_rejection` | `test_codec_clean_streaming` | `test_protocol_header.py` |
| 12 | Stream Name Matching | M2 | `test_stream_name_matching_ascii` | `test_stream_name_padding_and_nulls` | `test_stream_name_multicast_multiplex`| `test_stream_name_isolation_workload`| `test_protocol_header.py` |
| 13 | Exact Payload Size Calculation | M2 | `test_exact_payload_size_valid` | `test_payload_truncated_and_excess` | `test_payload_size_dynamic_format` | `test_payload_size_stress_stream` | `test_protocol_header.py` |
| 14 | PCM 16-bit Dequantization | M2 | `test_pcm16_normalization_scale` | `test_pcm16_min_max_extremes` | `test_pcm16_interleaved_stereo` | `test_pcm16_cd_quality_stream` | `test_dequantizers.py` |
| 15 | PCM 24-bit Packed Dequantization | M2 | `test_pcm24_packed_sign_extension` | `test_pcm24_min_max_0x800000_0x7FFFFF`| `test_pcm24_multichannel_unpacking` | `test_pcm24_studio_master_stream` | `test_dequantizers.py` |
| 16 | PCM 32-bit Dequantization | M2 | `test_pcm32_normalization_scale` | `test_pcm32_min_max_extremes` | `test_pcm32_interleaved_stereo` | `test_pcm32_highres_stream` | `test_dequantizers.py` |
| 17 | Float32 IEEE 754 Normalization | M2 | `test_float32_passthrough_clamping`| `test_float32_nan_inf_sanitization` | `test_float32_outlier_clipping` | `test_float32_768k_pure_sine` | `test_dequantizers.py` |
| 18 | Channel Deinterleaving / Interleaving| M2 | `test_channel_interleave_layout` | `test_channel_mono_to_8ch_layout` | `test_channel_remap_during_stream` | `test_channel_surround_fidelity` | `test_dequantizers.py` |
| 19 | Lock-Free SPSC Circular Buffer | M2 | `test_spsc_power_of_two_capacity` | `test_spsc_full_empty_overflow_bounds`| `test_spsc_concurrent_push_pull` | `test_spsc_sustained_streaming` | `test_spsc_buffer.py` |
| 20 | Oboe Low-Latency Stream Setup | M3 | `test_oboe_stream_builder_contract`| `test_oboe_unsupported_rate_error` | `test_oboe_exclusive_mode_fallback` | `test_oboe_low_latency_pipeline` | `test_bitperfect_audio_routing.py` |
| 21 | Real-Time Audio Callback Engine | M3 | `test_audio_callback_zero_allocation`| `test_audio_callback_underrun_silence`| `test_audio_callback_jitter_absorption`| `test_audio_callback_60fps_deadline` | `test_bitperfect_audio_routing.py` |
| 22 | Audio Error Recovery Callback | M3 | `test_error_callback_route_recovery`| `test_error_callback_dac_disconnect` | `test_error_callback_reopen_backoff`| `test_error_callback_hotplug_recover`| `test_bitperfect_audio_routing.py` |
| 23 | Clock Drift Jitter Filter | M3 | `test_jitter_filter_ema_convergence`| `test_jitter_filter_step_response` | `test_jitter_filter_under_packet_loss`| `test_jitter_filter_wifi_bursts` | `test_clock_drift_resampler.py` |
| 24 | Adaptive Clock Drift Servo | M3 | `test_drift_servo_pi_control` | `test_drift_servo_clamp_and_slew_cap`| `test_drift_servo_rate_compensation` | `test_drift_servo_long_session` | `test_clock_drift_resampler.py` |
| 25 | Catmull-Rom Micro-Resampler | M3 | `test_catmull_rom_spline_math` | `test_catmull_rom_phase_wraparound` | `test_catmull_rom_drift_modulation` | `test_catmull_rom_sine_fidelity` | `test_clock_drift_resampler.py` |
| 26 | Decoupled Stream Reconfiguration | M3 | `test_stream_reconfig_cosine_fade` | `test_stream_reconfig_rapid_changes` | `test_stream_reconfig_mid_stream` | `test_stream_reconfig_workload` | `test_clock_drift_resampler.py` |
| 27 | UDP Socket Network Engine | M4 | `test_udp_port_6980_binding` | `test_udp_socket_buffer_4mb_sizing` | `test_udp_high_packet_throughput` | `test_udp_bursty_network_stream` | `test_network_transport.py` |
| 28 | UDP Multicast Group Joining | M4 | `test_multicast_membership_join` | `test_multicast_invalid_group_reject`| `test_multicast_concurrent_unicast` | `test_multicast_multiroom_scenario` | `test_network_transport.py` |
| 29 | Packet Sequence & Loss Tracking | M4 | `test_nuframe_sequential_continuity`| `test_nuframe_rollover_0xffffffff` | `test_nuframe_loss_burst_reporting` | `test_nuframe_real_world_tracking` | `test_network_transport.py` |
| 30 | Inter-Arrival Jitter Estimation | M4 | `test_rfc3550_jitter_formula` | `test_rfc3550_jitter_spike_bounds` | `test_rfc3550_jitter_with_loss` | `test_rfc3550_wifi_jitter_recovery` | `test_network_transport.py` |
| 31 | Non-Blocking JNI Bridge | M4 | `test_jni_bridge_contract_signatures`| `test_jni_telemetry_snapshot_atomic` | `test_jni_engine_control_concurrency`| `test_jni_continuous_polling_load` | `test_telemetry_ui.py` |
| 32 | NativeVbanEngine Kotlin Wrapper | M4 | `test_kotlin_wrapper_state_machine` | `test_kotlin_wrapper_null_pointers` | `test_kotlin_wrapper_lifecycle_sync` | `test_kotlin_wrapper_stress_session` | `test_android_lifecycle_locks.py` |
| 33 | Android App & Compose Project | M5 | `test_app_manifest_and_theme_spec` | `test_target_sdk_and_min_sdk_bounds` | `test_compose_ui_state_flow_sync` | `test_app_full_lifecycle_simulation`| `test_android_lifecycle_locks.py` |
| 34 | Foreground Service (`mediaPlayback`) | M5 | `test_foreground_service_type_media` | `test_foreground_service_missing_type`| `test_foreground_service_notification`| `test_foreground_service_persistence` | `test_android_lifecycle_locks.py` |
| 35 | Triple Lock Power & Network Manager | M5 | `test_triple_lock_acquisition` | `test_triple_lock_double_release` | `test_triple_lock_screen_off_held` | `test_triple_lock_long_session` | `test_android_lifecycle_locks.py` |
| 36 | Android Permissions Handling | M5 | `test_permissions_post_notifications`| `test_permissions_denied_fallback` | `test_permissions_runtime_request` | `test_permissions_lifecycle_compliance`| `test_android_lifecycle_locks.py` |
| 37 | Android 14+ Bit-Perfect USB DAC | M5 | `test_bitperfect_mixer_selection` | `test_bitperfect_unsupported_device` | `test_bitperfect_hotplug_reactivity` | `test_bitperfect_audiophile_playback`| `test_bitperfect_audio_routing.py` |
| 38 | Android 13- AAudio Fallback | M5 | `test_aaudio_exclusive_fallback` | `test_aaudio_fallback_legacy_api` | `test_aaudio_shared_mode_resample` | `test_aaudio_fallback_real_playback` | `test_bitperfect_audio_routing.py` |
| 39 | Audio Device Enumeration & Selection | M5 | `test_device_enumeration_usb_types` | `test_device_hotplug_empty_list` | `test_device_switch_during_playback` | `test_device_auto_routing_workload` | `test_bitperfect_audio_routing.py` |
| 40 | Jetpack Compose Dashboard UI | M5 | `test_compose_dashboard_state_binding`| `test_compose_dashboard_empty_state` | `test_compose_dashboard_rate_update` | `test_compose_dashboard_ui_loop` | `test_telemetry_ui.py` |
| 41 | 60 FPS Stereo VU Meter Canvas | M5 | `test_vu_meter_dbfs_and_peak_decay` | `test_vu_meter_min_floor_and_clip` | `test_vu_meter_rapid_transients` | `test_vu_meter_continuous_60fps` | `test_telemetry_ui.py` |
| 42 | Jitter & Buffer Occupancy Gauge | M5 | `test_buffer_occupancy_gauge_math` | `test_buffer_gauge_underrun_overflow`| `test_buffer_gauge_burst_drift` | `test_buffer_gauge_real_wi_fi_jitter`| `test_telemetry_ui.py` |
| 43 | Real-Time Telemetry Counters | M5 | `test_telemetry_counters_accuracy` | `test_telemetry_counters_wrap_bounds` | `test_telemetry_counters_stream_switch`| `test_telemetry_sustained_telemetry` | `test_telemetry_ui.py` |
| 44 | E2E Test Suite Execution (Tiers 1-4) | M6 | `test_suite_execution_contract` | `test_suite_boundary_stress` | `test_suite_cross_feature_matrix` | `test_suite_tier4_workload_matrix` | `run_tests.py` |
| 45 | Adversarial Coverage Hardening (Tier 5)| M6 | `test_adversarial_malformed_packets`| `test_adversarial_fuzz_permutations` | `test_adversarial_stress_conditions` | `test_adversarial_session_longevity` | `tier2_boundary_corner/` |

---

## 3. Test Architecture & Runner Semantics

### Directory Layout
```
D:\Workspaces\vban-receptor\
├── TEST_INFRA.md                   # Complete test architecture & coverage matrix
├── TEST_READY.md                   # Test suite readiness declaration
└── e2e_tests/
    ├── __init__.py
    ├── run_tests.py                # Main test runner (Python 3)
    ├── run_tests.ps1               # PowerShell wrapper
    ├── vban_packet_factory.py      # Binary VBAN packet builder and fuzzer
    ├── audio_dsp_oracle.py         # Authoritative mathematical models (Dequantizers, Catmull-Rom, Jitter, dBFS)
    ├── spsc_ring_buffer_sim.py     # Lock-free SPSC circular buffer simulation model
    ├── tier1_feature_coverage/     # Tier 1: Happy path & interface contracts
    │   ├── __init__.py
    │   ├── test_protocol_header.py
    │   ├── test_dequantizers.py
    │   ├── test_spsc_buffer.py
    │   ├── test_clock_drift_resampler.py
    │   ├── test_network_transport.py
    │   ├── test_android_lifecycle_locks.py
    │   ├── test_bitperfect_audio_routing.py
    │   └── test_telemetry_ui.py
    ├── tier2_boundary_corner/      # Tier 2: Boundary & corner cases
    │   ├── __init__.py
    │   ├── test_packet_boundaries.py
    │   ├── test_numeric_extremes.py
    │   ├── test_sequence_wraparound.py
    │   ├── test_buffer_extremes.py
    │   └── test_drift_extremes.py
    ├── tier3_cross_feature/        # Tier 3: Pairwise cross-feature interactions
    │   ├── __init__.py
    │   └── test_pairwise_interactions.py
    └── tier4_real_world_scenarios/ # Tier 4: Realistic end-to-end streaming workloads
        ├── __init__.py
        └── test_real_world_workloads.py
```

### Execution Commands

```powershell
# Run entire E2E test suite (All Tiers 1-4)
python e2e_tests/run_tests.py

# Or via PowerShell script wrapper
.\e2e_tests\run_tests.ps1

# Run specific tier
python e2e_tests/run_tests.py --tier 1
python e2e_tests/run_tests.py --tier 2
python e2e_tests/run_tests.py --tier 3
python e2e_tests/run_tests.py --tier 4

# Run with verbose output
python e2e_tests/run_tests.py -v
```

### Exit Code Semantics
- **`0`**: **SUCCESS**. 100% of executed test cases passed with zero failures or errors.
- **`1`**: **TEST FAILURE**. One or more test assertions failed.
- **`2`**: **RUNTIME / SETUP ERROR**. Unhandled exception, syntax error, or environment issue occurred during test discovery or execution.
