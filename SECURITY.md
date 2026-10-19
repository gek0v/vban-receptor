# Security Policy

## Supported Versions

We provide security updates and critical fixes for the following versions of VBAN Receptor for Android:

| Version | Supported          |
| ------- | ------------------ |
| 1.0.x   | :white_check_mark: |
| < 1.0   | :x:                |

---

## Reporting a Vulnerability

The security and privacy of our users are of utmost importance. If you believe you have discovered a vulnerability, security bug, or memory-safety issue in VBAN Receptor for Android, please report it responsibly.

### How to Report:
- **Do not open a public issue.**
- Send a detailed advisory with reproduction steps, environment details, and proof-of-concept (if available) to:
  **`gekov.dev@gmail.com`** (or create a Private Vulnerability Report directly through GitHub Security Advisories).
- We will acknowledge receipt of your report within 48 hours and work with you on a timeline for a coordinated patch and public disclosure.

---

## Security Practices in VBAN Receptor

- **Strict Buffer Bounds Checking**: All incoming UDP network packets are strictly validated against minimum and maximum size boundaries before memory operations are performed.
- **Lock-Free Concurrency**: Ring buffers use monotonic unsigned 64-bit sequence counters and atomic acquire/release barriers to prevent race conditions and data corruption.
- **Permission Minimization**: Only permissions required for network reception (`INTERNET`, `ACCESS_WIFI_STATE`, `CHANGE_WIFI_MULTICAST_STATE`), CPU keep-alive (`WAKE_LOCK`), and foreground media playback (`FOREGROUND_SERVICE_MEDIA_PLAYBACK`) are requested.
