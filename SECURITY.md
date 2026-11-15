# Security Policy

This project interfaces directly with Linux kernel V4L2 character devices (`/dev/video*`) and DMA-BUF memory subsystems on Qualcomm Snapdragon hardware. Hardware acceleration operates with explicit memory layouts and buffer-sharing boundaries.

---

## Reporting Vulnerabilities

If you discover a security vulnerability, please report it responsibly:

1. **GitHub Security Advisories (Recommended)**: Use the "Report a vulnerability" button on the GitHub repository Security tab to open a private advisory.
2. **Private Issue**: If GitHub Security Advisories is unavailable, open a minimal issue containing only the driver version and a request for a private reporting channel. Do not attach exploitable media samples, memory dumps, or network credentials to public issues.

---

## Scope & Considerations

- **Buffer Boundaries**: Imported client DMA-BUFs are validated against strict stride and alignment constraints before queueing to hardware to prevent out-of-bounds VPU memory access.
- **Kernel & Firmware Boundaries**: The driver operates in user space as a dynamic library loaded by unprivileged applications (such as browsers). Host sandbox boundaries and kernel driver validation remain the authoritative security perimeter.
