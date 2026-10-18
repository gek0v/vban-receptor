## 2026-10-18T14:43:20Z
You are an Explorer agent for Milestone 2: VBAN Protocol Header & Parser Implementation Architecture.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m2_protocol
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Scope Document: D:\Workspaces\vban-receptor\.agents\teamwork\sub_orch_m2\SCOPE.md
- VBAN Spec Survey: D:\Workspaces\vban-receptor\.agents\teamwork\spec_miner_vban\survey_report.md

OBJECTIVE:
Investigate and produce complete, production-ready C++20 code templates for:
1. `app/src/main/cpp/include/vban_protocol.hpp`:
   - Exact 28-byte packed `VBanHeader` struct matching specification.
   - Constants: FourCC magic `0x4E414256`, subprotocols (Audio `0x00`), codecs (PCM `0x00`), data types.
   - Sample rate table (`VBAN_SR_TABLE`) with 21 official rates and 768 kHz extension.
   - Helper functions: `getSampleRate()`, `getSampleSize()`, `calculatePayloadBytes()`.
2. `app/src/main/cpp/include/vban_parser.hpp` and `src/vban_parser.cpp`:
   - `VbanParser` class implementing the strict 10-step packet validation pipeline.
   - Validation against minimum packet size (28 bytes), FourCC, subprotocol, codec, reserved bit 3, sample rate index, data type, stream name match, and exact payload size.
   - Sequence number (`nuFrame`) loss tracking and RFC 3550 jitter calculation.
3. Unit test template `native_tests/test_vban_protocol.cpp` exercising all features and edge cases.
4. Output: Write detailed findings and complete code templates to D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m2_protocol\survey_report.md and handoff.md. Send completion message to parent.
