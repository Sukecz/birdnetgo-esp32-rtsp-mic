# Changelog

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
