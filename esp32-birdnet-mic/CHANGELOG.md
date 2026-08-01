# Changelog

## 1.22 - 2026-08-01
- RTSP/TCP stability: RTP writes now use a genuinely non-blocking socket send with an explicit
  ten-second recovery window. Live testing showed that shorter limits converted temporary
  Wi-Fi/TCP interruptions into much longer FFmpeg reconnects; the restored tolerance minimizes
  complete stream drops while retaining controlled error handling. The socket
  stall is treated as the consequence of the transport interruption, not as its initiating cause.
- Live-audio recovery: after a recovered stall longer than 250 ms, or after a failed client, queued
  stale audio is flushed so the stream resumes at the live edge instead of retaining latency.
- Thermal stability: restore the default Wi-Fi TX power to 15 dBm after C6 testing at 19.5 dBm
  raised normal afternoon die temperature by roughly 8–11 C without eliminating transport stalls.
- Thermal protection: reject isolated internal-sensor spikes by requiring three consecutive
  five-second samples at the configured shutdown limit, while an excursion 10 C above the limit
  still stops the stream immediately.

## 1.21 - 2026-07-22
- PCM1808 preparation: I2S now outputs a 256-fs master clock on physical XIAO pin D7 while
  continuing to capture the left slot as mono audio. Existing ICS-43434 and INMP441 wiring remains
  unchanged because those microphones do not connect to D7.
- Board compatibility: MCLK uses the symbolic D7 mapping for every supported XIAO C3/S3/C5/C6
  target instead of assuming a shared GPIO number.
- Contribution: PCM1808/MCLK support was proposed by
  [@dalogue1](https://github.com/dalogue1); the complete analog microphone path remains pending
  hardware validation.
- MQTT resilience: an interrupted broker connection now retries every five minutes even during an
  active RTSP stream, using a cached broker address and a 500 ms TCP connection limit.
- Streaming safety: reconnect restores availability, commands, and current state immediately, while
  the larger Home Assistant discovery refresh remains deferred until streaming stops.

## 1.20 - 2026-07-21
- Version-only test build for validating the improved OTA installation progress and automatic
  browser reconnection workflow from v1.18. There are no functional changes compared with v1.18.

## 1.18 - 2026-07-21
- OTA UI: installation now immediately shows a blocking progress overlay, warns before leaving the
  page, reports the reboot stage, and automatically reconnects when the device returns.
- RTSP diagnostics: client-requested TEARDOWN events now include client, slot, stream, transport,
  CSeq, session and stream age, packet count, last RTP age, Wi-Fi RSSI, and client User-Agent.

## 1.17 - 2026-07-21
- Version-only test build for validating the complete Web UI automatic OTA workflow from v1.16.
  There are no functional firmware changes compared with v1.16.

## 1.16 - 2026-07-21
- Audio safety: configuration changes now stop the producer before publishing new producer-visible
  values, preventing a larger requested block from using the previous smaller allocation.
- Offline resilience: automatic firmware checks run outside the RTSP loop, and MQTT reconnects are
  deferred while streaming so unavailable network services cannot block audio delivery.
- HPF validation: cutoff is limited to 45% of the active sample rate and stored consistently, avoiding
  coefficient recalculation and filter-state resets on every audio block.
- Boot safety: persisted settings are validated and repaired before buffers, I2S, CPU, schedules, or
  MQTT are initialized. Factory defaults now explicitly restore Time Sync to enabled.
- Defaults: gain is now 1.5 while the 512-sample buffer remains unchanged.
- Web flasher: ESP Web Tools 10.4.0 is self-hosted, unique mDNS wording is corrected, and the public
  site gains HTTPS redirect and browser security headers without redirecting HTTP OTA artifacts.

## 1.15 - 2026-07-21
- OTA safety: automatic installation now fetches and validates `ota-version.txt`, compares strict
  `major.minor` versions, and downloads the board-specific image only when the published version is
  newer than the installed firmware.
- OTA UI: added explicit update-available, up-to-date, and check-failed states. The main Web UI shows
  a prominent update banner and highlighted firmware link when a newer version is confirmed.
- Offline behavior: version checks use bounded network timeouts, failures are cached for one hour,
  the main page checks only once per page session, and manual app-only upload remains usable.

## 1.14 - 2026-07-21
- Diagnostics: fixed 32-bit overflow in global, per-stream, and MQTT packet-rate calculations. At
  48 kHz with 512-sample packets the old expression overflowed after about 12.7 hours, falsely
  triggering daily I2S auto-recovery despite zero I2S read errors.
- Audio buffering: increased the internal ring-buffer minimum from 8 KiB to 32 KiB for packet sizes
  up to 2048 samples. The public/default RTP packet remains 512 samples for BirdNET-Pi/UDP safety.
- RTSP/TCP resilience: extended the bounded write retry window from 30 ms to 100 ms.
- RTSP diagnostics: write failures now log partial byte progress, elapsed time, retry count, TCP
  write availability, ring-buffer use, connection state, and Wi-Fi RSSI.

## 1.13 - 2026-07-19
- I2S: migrated microphone capture from the deprecated legacy driver to the current channel-based
  `i2s_std` API while preserving the existing BCLK/WS/SD wiring and leaving MCLK unused.
- RTSP/TCP: combined the interleaved framing header, RTP header, and PCM payload into one application
  buffer before writing to reduce TCP-stack calls per RTP packet.
- Web UI: limited synchronous client handling to 50 Hz so an open browser connection cannot dominate
  the main streaming loop.
- Compatibility: retained the 512-sample default packet buffer validated with BirdNET-Pi/UDP.

## 1.12 - 2026-07-10
- OTA discovery: automatic update now downloads the board-specific stable latest alias instead of
  constructing a versioned URL from the currently installed `FW_VERSION`.
- OTA board safety: the pull URL is compiled into each supported C3/S3/C5/C6 profile, is no longer
  editable in the Web UI, and must match exactly. Generic builds no longer fall back to a C6 image.
- OTA download safety: validates HTTP 200, binary content type, positive content length, app
  partition capacity, exact byte count, timeout, premature disconnect, and Update completion.
- OTA diagnostics: `/api/status` exposes the compiled OTA artifact and automatic-update support.
- Release validation: added an OTA contract test plus local/public equality and checksum checks for
  stable, versioned, manual, and C6 compatibility artifacts.

## 1.11 - 2026-07-10
- I2S wiring: fixed XIAO ESP32-C3/S3/C5 builds to use their documented `D3`/`D1`/`D2` GPIO
  mappings. Arduino defines these pin labels as C++ constants, so the previous `defined(D1)`
  preprocessor check always selected the C6 fallback GPIOs (`21/1/2`).
- Diagnostics: I2S startup logs now report the active BCLK/WS/SD GPIO numbers.
- Web flasher and OTA: regenerated all C3/S3/C5/C6 images with the corrected pin selection.

## 1.10.1 - 2026-06-25
- OTA security: `/ota/install` and `/ota/upload` now require the same mutation header as other
  state-changing Web UI/API endpoints.
- OTA safety: automatic pull updates are limited to official `firmware-app*.bin` URLs, and OTA now
  rejects oversized or merged USB firmware images for 4 MB and 8 MB targets.
- OTA maintenance: board-specific automatic update URLs are generated from the compiled
  `FW_VERSION` instead of hard-coded release strings.
- CI: GitHub Actions now compile-checks all supported XIAO ESP32-C3/S3/C5/C6 firmware targets.

## 1.10.0 — 2026-06-11
- Board support: added compile-time profiles for Seeed Studio XIAO ESP32-C3, XIAO ESP32-S3,
  XIAO ESP32-C5, and XIAO ESP32-C6.
- I2S wiring: switched the default microphone pins to XIAO physical labels `D3`/`D1`/`D2`, keeping
  the same physical wiring across supported XIAO boards while allowing each Arduino variant to map
  to its own GPIO numbers.
- Antenna handling: kept GPIO3/GPIO14 external antenna RF-switch control only for XIAO ESP32-C6.
  C3/S3/C5 builds do not drive antenna-control GPIOs.
- Metadata: `/api/status`, MQTT device metadata, and the OTA page now expose board id, board name,
  and chip family.
- OTA: automatic update URL is selected by compiled board profile; `firmware-app.bin` remains the
  compatibility alias for the XIAO ESP32-C6 app image.
- Web flasher: manifest now contains separate ESP32-C3, ESP32-S3, ESP32-C5, and ESP32-C6 builds so
  ESP Web Tools can auto-select firmware by connected chip family.
- Web flasher artifacts: generated `firmware-app-*.bin`, `firmware-*.bin`, `bootloader-*.bin`,
  `partitions-*.bin`, and `boot_app0-*.bin` for C3/S3/C5/C6.
- Validation: compile-verified with Arduino ESP32 core 3.3.8 on
  `esp32:esp32:XIAO_ESP32C3`, `esp32:esp32:XIAO_ESP32S3`, `esp32:esp32:XIAO_ESP32C5`, and
  `esp32:esp32:XIAO_ESP32C6`. Runtime hardware validation remains primarily on XIAO ESP32-C6.

## 1.9.3 — 2026-06-11
- Defaults: changed the packet buffer default from 1024 to 512 samples after BirdNET-Pi UDP testing
  showed stutter with 1024-sample packets.
- RTSP/UDP compatibility: opened the advertised RTCP server port, drained incoming RTCP packets,
  and added `source`, `ssrc`, and `RTP-Info` metadata so ffmpeg/BirdNET-Pi handles UDP RTP streams
  more reliably.
- Validation: clean Nachtzuster/BirdNET-Pi host install tested with `rtsp://192.168.1.80:8554/audio2`
  in BirdNET-Pi/UDP mode; ffmpeg produced full 15-second WAV files and BirdNET-Pi analysis read
  5 audio chunks per segment.
- Audio settings: sample rate validation now accepts up to 192000 Hz in both Web UI and API.
- Audio settings: gain, sample rate, buffer, and I2S shift are now persisted only after the I2S
  pipeline restarts successfully; failed audio changes roll back instead of rebooting or silently
  reverting later.
- Web UI: fixed dirty-field highlighting so saved stream/client/CPU/auto-recovery fields clear
  correctly after polling or a successful save, and failed saves remain marked as unsaved.
- Web flasher artifacts: version bumped to 1.9.3; `firmware.bin`, `firmware-app.bin`, and
  `ota-version.txt` are regenerated for publishing.
- Docs: refreshed default audio settings and public firmware artifact notes.

## 1.9.2 — 2026-05-16
- Web UI: added `/ota` firmware update page.
- OTA page: added automatic update from `http://esp32mic.msmeteo.cz/firmware-app.bin`.
- OTA page: added manual upload for app-only `.bin` builds.
- OTA note: automatic download intentionally uses plain HTTP because HTTPS/TLS does not fit in the
  default XIAO ESP32-C6 app partition together with the existing features.
- Web flasher: added OTA section where users enter the device IP and open the device update page.
- Web flasher artifacts: version bumped to 1.9.2; `firmware.bin`, `firmware-app.bin`, and `ota-version.txt`
  are generated for publishing.
- Docs: simplified user instructions and documented that the default public build uses the external
  XIAO ESP32-C6 antenna path and has no OTA password.

## 1.9.1 — 2026-05-15
- Build size: added `build_opt.h` for the ESP32 Arduino build system to compile without unused C++
  exception/unwind metadata.
- XIAO ESP32-C6 default partition: verified `esp32:esp32:XIAO_ESP32C6` now builds at 1,244,584 bytes
  of 1,310,720 bytes in `arduino-cli`, leaving about 66 KB reserve before Arduino IDE-specific
  variation.
- Scope: no firmware features were removed; RTSP streams, Web UI, API, MQTT/Home Assistant discovery,
  OTA, diagnostics, schedules, deep sleep, and thermal protection remain enabled.
- Web flasher: manifest version updated to 1.9.1 and `firmware.bin` regenerated from the current
  merged Arduino build.

## 1.9.0 — 2026-05-09
- Audio pipeline: I2S capture now runs in a dedicated FreeRTOS producer task.
- Audio pipeline: processed PCM blocks are passed to the RTSP sender through a FreeRTOS ring buffer, reducing sensitivity to short Wi-Fi/client write stalls while keeping RTSP/RTP output unchanged.
- RTSP: first PLAY after an idle period flushes stale queued audio before starting a fresh stream.
- Diagnostics: `/api/audio_status` now exposes producer state, ring-buffer capacity, queued chunks, ring-buffer drops/flushes, I2S read errors, and RTSP write stall/timeout counters.
- Reliability: restart/reset I2S now stops the producer task and deletes the ring buffer before reallocating audio buffers.


## 1.8.0 — 2026-05-08
- RTSP: dual stream endpoints `/audio` (stream 1, legacy `/audio1` alias) and `/audio2`, each independently enable/disable.
- RTSP: server internals migrated from single-client to configurable 1-3 concurrent sessions (default 2).
- Transport: auto-derived from backend target (TCP for BirdNET-Go, UDP for BirdNET-Pi), removed manual selector.
- Backend routing: per-stream target selector BirdNET-Go / BirdNET-Pi persisted in NVS.
- Per-stream live stats: client count, streaming state, packet rate, last play time.
- MQTT/Home Assistant: discovery now exposes stream 1 and stream 2 separately, including enable switches, streaming status, client counts, packet rates, RTSP URLs, and BirdNET target selects.
- MQTT: reconnect now allowed during streaming with longer interval (120s) instead of blocking indefinitely.
- mDNS: fixed hostname collision on ESP32-C6 — `buildMqttMacSuffix()` now uses `WiFi.macAddress()` (big-endian) instead of `ESP.getEfuseMac()` (little-endian), which caused boards from the same vendor to share the same default hostname.
- API: `/api/status` exposes per-stream URLs, enabled state, max_clients, and live stats.
- API settings: `stream1_enabled`, `stream2_enabled`, `max_clients`, `stream1_target`, `stream2_target`.
- UI: unified Status card with system info + two stream columns (config + live data).
- UI: removed hero RTSP URLs (duplicated in Status card).
- UI: Max Clients selector (1/2/3) in Advanced Settings.

### 1.8.0 Test status
- Done:
  - Build/compile on XIAO ESP32-C6 target.
  - API fields for dual stream present in firmware.
- Pending (next validation cycle):
  - End-to-end validation with BirdNET-Go on `/audio`.
  - End-to-end validation with BirdNET-Pi on `/audio2`.
  - Mixed transport test (one TCP stream + one UDP stream concurrently).
  - UI validation for selector persistence after reboot and NVS reload.

## 1.7.0 — 2026-04-29
- WiFi Reconnect: new API endpoint `/api/action/wifi_reconnect` with optional BSSID pinning for manual WiFi reconnection without reboot.
- WiFi Reconnect: new "Reconnect Wi-Fi" button in Web UI (Time & Network section).
- Deep sleep: explicit MQTT offline publish before disconnect for faster Home Assistant detection (independent of LWT timeout).
- Deep sleep: added `mqttClient.connected()` guard before MQTT operations.
- Docs: added INMP441 as a user-reported compatible I2S microphone using the same pins as the
  ICS-43434 reference mic (`SCK` -> GPIO21, `WS` -> GPIO1, `SD` -> GPIO2).
- Docs: clarified that INMP441 `L/R` or `SEL` should be set to the left channel because the
  firmware reads `ONLY_LEFT`.
- Web flasher: manifest version updated to 1.7.0 and `firmware.bin` regenerated from the current
  merged build.

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
