# Changelog

## 1.6.0 — 2026-02-13
- MQTT: publish interval is now configurable in UI/API (`mqtt_interval`), persisted in NVS (`mqttIntSec`), default `60 s` (range `10..3600`).
- MQTT state payload extended with diagnostics: `fw_build`, `reboot_reason`, `restart_counter`, `wifi_ssid`, `wifi_reconnect_count`, `stream_uptime_s`, `client_count`, `audio_format`.
- MQTT Discovery (Home Assistant): added entities for new diagnostics (build date, reboot reason, restart counter, Wi-Fi reconnects/SSID, stream uptime, client count, sample rate, audio format).
- MQTT behavior: immediate state publish on key events (Wi-Fi reconnect, stream start/stop, RTSP client connect/disconnect/timeout, schedule/thermal stream stop), while keeping periodic publish interval.
- Boot diagnostics: reset reason is detected at boot and restart counter is incremented and stored in NVS.
- API hardening: mutating endpoints now use `POST` only and require header `X-ESP32MIC-CSRF: 1`; UI settings/actions were updated accordingly.
- API validation: numeric setting parsing is now strict (invalid numeric strings are rejected instead of being coerced).
- Firmware version bumped to 1.6.0.

## 1.5.0 — 2026-02-11
- Time & Network: added stream schedule (start/stop local time window) with support for overnight ranges (for example 22:00-06:00).
- Schedule policy: fail-open when local time is unavailable (stream remains allowed instead of being blocked).
- Schedule rule update: `stream_start_min == stream_stop_min` is treated as an explicit empty window (stream blocked always), independent of time-sync validity.
- Time & Network: added optional deep sleep mode outside the stream schedule window (`Deep Sleep (Outside Window)`).
- Safety policy for deep sleep is conservative to avoid loops and lockouts: requires valid synchronized time, startup grace delay, outside-window stabilization delay, and no active stream/client.
- Deep sleep is blocked when time is unavailable (unsynced); stream schedule still keeps fail-open behavior.
- Max one deep-sleep cycle set to 8 hours.
- Added 5-minute wake guard before stream window start to absorb RTC drift and avoid edge sleep/wake flapping.
- Deep-sleep diagnostics: after timer wake, firmware logs a retained sleep snapshot (cycle, planned sleep, entered time, schedule window, seconds-until-start at sleep, offset).
- UI: added Stream Schedule controls, Schedule Status row, and tooltip help for new fields.
- UI: added tooltip help and double-confirmation dialog when enabling deep sleep.
- UI/API: new deep sleep status row and status fields (`deep_sleep_sched_enabled`, `deep_sleep_status_code`, `deep_sleep_next_sec`).
- UI: Time & Network now displays current Local and UTC time (using configured offset) for quick verification.
- UI logs: autoscroll behavior improved; manual scrolling up no longer jumps back to bottom on periodic refresh.
- UI mobile: top header/RTSP links now wrap correctly, preventing horizontal overflow scrollbar on narrow screens.
- API: `/api/status` now exposes `stream_schedule_enabled`, `stream_schedule_start_min`, `stream_schedule_stop_min`, `stream_schedule_allow_now`, and `stream_schedule_time_valid`.
- API settings: new keys `stream_sched`, `stream_start_min`, `stream_stop_min`, `deep_sleep_sched`.
- Persistence: new NVS key `deepSchSlp`.
- Firmware version bumped to 1.5.0.

## 1.4.0 — 2026-02-09
- Time: NTP sync on boot and every 30 min when internet is reachable; manual time offset (minutes) stored in NVS; logs fall back to uptime when offline.
- Logs: ring buffer enlarged to 120 lines, each line timestamped; one-click download as text from the Web UI.
- Network: mDNS hostname `esp32mic.local` (toggle in UI); RTSP links show both IP and mDNS; Wi-Fi credentials reset action added to UI.
- UI: new Time & Network card, stream URLs moved to header, firmware version bumped to 1.4.0.
- Docs: README updated with mDNS, time sync/offset, log download, and network reset notes.
- Sync logic refined: unsynced retry every hour, synced refresh every 6 h; optional Time Sync ON/OFF in UI/NVS; OTA hostname unified with mDNS (`esp32mic.local`).

## 1.3.0 — 2025-09-09
- Thermal protection: added configurable shutdown limit (30–95 °C, default 80 °C) with protection enabled by default.
- Thermal latch now persists across reboots and must be acknowledged in the Web UI before RTSP can be re-enabled; UI includes clear button and richer status strings.
- Firmware: on overheat the RTSP server is stopped, the reason/temperature/timestamp are persisted, and a manual restart is required.
- Web UI: Thermal card now exposes the protection toggle, limit selector, status badge, last shutdown log, and detailed tooltips.
- Docs: refreshed defaults and added guidance for the new thermal workflow.

## 1.2.0 — 2025-09-08
- Added configurable High‑pass filter (HPF) to reduce low‑frequency rumble
- Web UI: Signal level meter with clip warning and beginner guidance
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
- Web UI on port 80 (English)
- JSON API endpoints (status, audio, performance, thermal, logs, actions, settings)
- In-memory log buffer, performance diagnostics, auto-recovery
- OTA and WiFiManager included
