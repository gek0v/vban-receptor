## 2026-10-18T16:24:03Z
You are an Explorer agent for Milestone 4: UDP Socket, Multicast & Network Receiver Architecture.
Your working directory is: D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_network
Your parent orchestrator conversation ID is: 9bb0c473-fb0a-499a-bbf2-a430786d0058

MANDATORY INPUTS:
- User original request: D:\Workspaces\vban-receptor\.agents\teamwork\ORIGINAL_REQUEST.md
- Project Architecture & Feature Inventory: D:\Workspaces\vban-receptor\PROJECT.md
- Scope Document: D:\Workspaces\vban-receptor\PROJECT.md § Interface Contracts & Milestones (M4)

OBJECTIVE:
Investigate and design production-ready C++20 code templates for the low-level UDP network socket engine (`vban_receiver.hpp`, `vban_receiver.cpp`):
1. UDP Socket Creation & Configuration:
   - Create UDP socket (`AF_INET`, `SOCK_DGRAM`, `IPPROTO_UDP`).
   - Set `SO_REUSEADDR` (and `SO_REUSEPORT` where available on Linux/Android).
   - Set `SO_RCVBUF` to 4 MB (4,194,304 bytes) with fallback/query via `getsockopt`.
   - Bind to specified port (default 6980) and listen IP (or INADDR_ANY if empty or "0.0.0.0").
2. Multicast Group Joining:
   - Detect if `listenIp` is in IPv4 multicast range (224.0.0.0 to 239.255.255.255).
   - Configure `ip_mreq` struct and execute `setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))`.
3. Non-Blocking / Timeout Operation:
   - Set receive timeout `SO_RCVTIMEO` (e.g. 100 ms) or use `poll()`/`select()` to prevent blocking indefinitely, allowing clean thread cancellation when `stop()` is called.
   - Cross-platform socket abstractions (NDK POSIX BSD sockets for Android, Winsock `ws2_32` guards for host tests).
4. Output:
   - Write comprehensive survey findings and C++20 class templates to D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_network\survey_report.md.
   - Write handoff report to D:\Workspaces\vban-receptor\.agents\teamwork\explorer_m4_network\handoff.md.
   - Update progress.md and notify parent orchestrator.
Do NOT modify any source code files directly outside your directory.
