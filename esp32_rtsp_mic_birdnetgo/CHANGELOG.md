# Changelog

## 1.4.0 — 2026-02-09
- Time: NTP sync on boot and every 30 min when internet is reachable; manual time offset (minutes) stored in NVS; logs fall back to uptime when offline.
- Logs: ring buffer enlarged to 120 lines, each line timestamped; one-click download as text from the Web UI.
- Network: mDNS hostname `esp32mic.local` (toggle in UI); RTSP links show both IP and mDNS; Wi-Fi credentials reset action added to UI.
- UI: new Time & Network card (EN/CZ), stream URLs moved to header, firmware version bumped to 1.4.0.
- Docs: README updated with mDNS, time sync/offset, log download, and network reset notes.
- Sync logic refined: unsynced retry every hour, synced refresh every 6 h; optional Time Sync ON/OFF in UI/NVS; OTA hostname unified with mDNS (`esp32mic.local`).

## 1.3.0 — 2025-09-09
- Thermal protection: added configurable shutdown limit (30–95 °C, default 80 °C) with protection enabled by default.
- Thermal latch now persists across reboots and must be acknowledged in the Web UI before RTSP can be re-enabled; UI includes clear button and richer status strings.
- Firmware: on overheat the RTSP server is stopped, the reason/temperature/timestamp are persisted, and a manual restart is required.
- Web UI: Thermal card now exposes the protection toggle, limit selector, status badge, last shutdown log, and detailed EN/CZ tooltips.
- Docs: refreshed defaults and added guidance for the new thermal workflow.

## 1.2.0 — 2025-09-08
- Added configurable High‑pass filter (HPF) to reduce low‑frequency rumble
- Web UI: Signal level meter with clip warning and beginner guidance (EN/CZ)
- RTSP: respond to `GET_PARAMETER` (keep‑alive) for better client compatibility
- API: `/api/status` now includes `fw_version`
- Docs: README updated (defaults, HPF notes, RTSP keep‑alive)
- Cleanup: removed unused arpa/inet dependency from source
- Defaults: Gain 1.2, HPF ON at 500 Hz

## 1.1.0 — 2025-09-05
- Web UI redesign: responsive grid, dark theme, cleaner cards
- Simplified controls: removed client Start/Stop/Disconnect; Server ON/OFF only
- Inline editing: change Sample Rate, Gain, Buffer, TX Power directly in fields
- Reliability: Auto/Manual threshold mode with auto‑computed min packet‑rate
- New settings: Scheduled reset (ON/OFF + hours), CPU frequency (MHz)
- Logs: larger panel; every UI action and setting change is logged
- Performance: faster initial load; immediate apply on Enter/blur
- Thermal: removed periodic temperature logging (kept high‑temp warning)

## 1.0.0 (Initial public release)
- Web UI on port 80 (English/Czech)
- JSON API endpoints (status, audio, performance, thermal, logs, actions, settings)
- In-memory log buffer, performance diagnostics, auto-recovery
- OTA and WiFiManager included
