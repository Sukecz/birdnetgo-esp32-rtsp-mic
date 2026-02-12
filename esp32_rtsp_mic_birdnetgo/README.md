<p align="center">
  <img src="../birdlogo.png" alt="ESP32 RTSP Mic for BirdNET-Go" width="240" />
</p>

# ESP32 RTSP Mic for BirdNET-Go (Firmware)

This folder contains the Arduino firmware for an ESP32-C6 + I2S MEMS microphone (ICS-43434)
that streams **mono 16-bit PCM** audio over **RTSP** and exposes an English Web UI plus a JSON API.

- Beginner-friendly overview + wiring: `../README.md`
- Firmware versions / changes: `CHANGELOG.md`
- Latest firmware: **v1.5.0** (2026-02-11)
- One-click web flasher: **https://esp32mic.msmeteo.cz**

---

## TL;DR

- Web UI: `http://<device-ip>/` (port **80**)
- RTSP audio: `rtsp://<device-ip>:8554/audio`
  (or `rtsp://esp32mic.local:8554/audio` when mDNS is enabled)
- Board: Seeed Studio **XIAO ESP32-C6** (tested)
- Mic: **ICS-43434** (I2S, mono)
- Defaults: 48 kHz, gain 1.2, buffer 1024, Wi-Fi TX ~19.5 dBm, shiftBits 12, HPF ON (500 Hz),
  CPU 160 MHz, thermal shutdown 80 C (protection ON)
- First boot: WiFiManager AP **ESP32-RTSP-Mic-AP** (open) + setup portal at `192.168.4.1`
- Limitation: **single RTSP client** at a time

---

## What's New (v1.5.0)

- Stream schedule in Time & Network: set RTSP allowed window by local time (`Start` / `Stop`).
- Overnight schedule windows are supported (for example `22:00-06:00`).
- If local time is not available (no valid sync), schedule runs in **fail-open** mode (stream remains allowed).
- Schedule edge-case rule: `Start == Stop` is treated as an explicit empty window (stream blocked always).
- New schedule status row in UI + help tooltips (`?`) for all schedule controls.
- New optional `Deep Sleep (Outside Window)` toggle in Time & Network with tooltip and double confirmation.
- Deep sleep uses conservative guards: only outside schedule window, only with valid time, startup grace delay, and no active stream/client.
- Max one deep-sleep cycle is 8 hours (device wakes, re-checks schedule/time, then can sleep again).
- Device wakes with a 5-minute guard before next stream window to reduce RTC-drift edge effects.
- New deep-sleep status row in UI explains why sleep is blocked/armed and shows next wake interval.
- After deep-sleep timer wake, logs show a retained snapshot (cycle, planned sleep, entered time, schedule window, offset).
- Time & Network shows current Local time and UTC time using the configured offset.
- Logs panel UX improved: when you scroll up, it no longer jumps back to bottom every refresh.
- Mobile header layout improved: top bar/RTSP links wrap without horizontal overflow.

---

## One-click Web Flasher (Recommended)

- Open **https://esp32mic.msmeteo.cz** in **Chrome or Edge on desktop**.
- Click **Flash**, pick the USB JTAG/serial device, wait for upload and reboot.
- After flashing, connect to AP **ESP32-RTSP-Mic-AP** (open).
  The captive portal should appear; if it does not, open `192.168.4.1`.
- Enter your Wi-Fi SSID/password, save. The device reboots and joins your Wi-Fi.

Tip: use a USB-C *data* cable (not charge-only) and avoid USB hubs for flashing.

---

## Wiring

![Wiring / pinout](../connection.png)

### I2S (ICS-43434 <-> ESP32-C6)

| ICS-43434 signal | ESP32-C6 GPIO | Notes |
|---:|:--:|---|
| **BCLK / SCK** | **21** | `#define I2S_BCLK_PIN 21` |
| **LRCLK / WS** | **1**  | `#define I2S_LRCLK_PIN 1` |
| **SD (DOUT)**  | **2**  | `#define I2S_DOUT_PIN 2` |
| **VDD**        | 3V3    | Power |
| **GND**        | GND    | Ground |

- I2S mode: **Master / RX**, reads **32-bit** samples, **ONLY_LEFT** channel; then shifts/scales to
  16-bit PCM.
- DMA: 8 buffers, `buf_len = min(bufferSize, 512)` samples.

### Antenna control (XIAO ESP32-C6)

- GPIO3 -> LOW (RF switch control enabled)
- GPIO14 -> HIGH (select external antenna)

If you use a different ESP32 board or you keep the internal antenna, comment out the GPIO3/GPIO14
block in `setup()` so the firmware does not drive pins your hardware does not have.

---

## First Boot & Network

- Wi-Fi power save is **disabled** (`WiFi.setSleep(false)`) for stable streaming.
- WiFiManager:
  - AP: `ESP32-RTSP-Mic-AP`
  - connect timeout: 60 s
  - portal timeout: 180 s
- Web UI: `http://<device-ip>/`
- RTSP (VLC/ffplay/BirdNET-Go):
  - `rtsp://<device-ip>:8554/audio`
  - `rtsp://esp32mic.local:8554/audio` (only when mDNS is enabled and available on your LAN)

### mDNS notes

- mDNS can be toggled in the Web UI.
- mDNS often fails on guest/isolated Wi-Fi networks (multicast blocked). If in doubt, use the IP.
- If you run BirdNET-Go in Docker, mDNS name resolution may not work inside the container; use the
  device IP (or a DHCP reservation) instead.

### Time sync + logs

- NTP sync is attempted on boot. If unsynced, it retries every **1 hour**; once synced, it refreshes
  every **6 hours**.
- You can turn time sync OFF in the Web UI (useful if the device has no internet access).
- Time Offset is set in **hours** in the UI (stored internally as minutes in NVS).
- Stream Schedule (Time & Network): if enabled, RTSP server is active only in the configured local-time window.
  Cross-midnight windows are supported; if time is invalid, fail-open keeps stream allowed.
  `Start == Stop` is an explicit empty window and keeps stream blocked even without valid time.
- Optional Deep Sleep (Time & Network): if enabled, device can sleep only outside stream window and only with valid time;
  when time is unsynced, deep sleep stays blocked (stream remains available by fail-open policy).
- After timer wake from deep sleep, startup logs include one retained sleep summary line for overnight verification.
- When the device has no valid time, logs fall back to **uptime** timestamps.
- Logs: 120-line ring buffer in the UI + one-click download as text.
- Logs panel keeps manual scroll position while browsing older entries.

---

## Verify The Stream

- VLC: *Media* -> *Open Network Stream* -> paste the RTSP URL.
- ffplay:
  `ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/audio`
- ffprobe:
  `ffprobe -rtsp_transport tcp rtsp://<device-ip>:8554/audio`

If VLC/ffplay works, use the same RTSP URL in BirdNET-Go.

---

## Architecture (Quick Map)

- MCU: ESP32-C6 (Seeed XIAO ESP32-C6 reference)
- Input: I2S MEMS mic (ICS-43434 reference)
- Output: RTSP server on port **8554** -> `audio` track, **L16/mono/16-bit PCM**
  - RTP dynamic PT 96, `rtpmap:96 L16/<sample-rate>/1`
  - Transport: `RTP/AVP/TCP;interleaved=0-1`
  - Keep-alive: RTSP `GET_PARAMETER` supported
- Control: Web UI (English) + JSON API (status, audio, perf/thermal, logs, actions, settings)
- Reliability: watchdogs + auto-recovery when packet-rate drops below threshold
- OTA: optional; protect with a password if enabled
- Timeouts: RTSP inactivity timeout ~30 s; Wi-Fi health checks and reconnection

---

## Build & Flash (Manual)

### Arduino IDE

1. Open `esp32_rtsp_mic_birdnetgo/esp32_rtsp_mic_birdnetgo.ino`.
2. Install ESP32 Arduino core with ESP32-C6 support.
3. Select board: *Seeed XIAO ESP32-C6* (or *ESP32-C6 Dev Module*).
4. Compile & upload over USB-UART.

### PlatformIO (VS Code)

- Framework: Arduino
- Platform: espressif32
- Target: ESP32-C6 (consider `env:xiao_esp32c6`)
- Typical: `pio run -t upload`

---

## Configuration

### Compile-time defaults

```c
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_GAIN_FACTOR 1.2f
#define DEFAULT_BUFFER_SIZE 1024
#define DEFAULT_WIFI_TX_DBM 19.5f

// I2S pins:
#define I2S_BCLK_PIN 21
#define I2S_LRCLK_PIN 1
#define I2S_DOUT_PIN 2

// High-pass defaults
#define DEFAULT_HPF_ENABLED true
#define DEFAULT_HPF_CUTOFF_HZ 500

// Thermal protection
#define DEFAULT_OVERHEAT_PROTECTION true
#define DEFAULT_OVERHEAT_LIMIT_C 80
```

### Runtime (persisted in NVS via Preferences)

Namespace: `"audio"`.

Audio:
- `sampleRate` (Hz) - default 48000
- `gainFactor` - default 1.2
- `bufferSize` (samples) - default 1024
- `shiftBits` - default 12 on first boot
- `hpEnable` - default true
- `hpCutoff` (Hz) - default 500

Reliability:
- `autoRecovery` - default true
- `thrAuto` - default true (auto/manual threshold mode)
- `minRate` (pkt/s) - default 50
- `checkInterval` (minutes) - default 15
- `schedReset` - default false
- `resetHours` - default 24

Network / time:
- `wifiTxDbm` (dBm) - default 19.5
- `mdnsEn` - default true
- `timeSyncEn` - default true
- `timeOffset` (minutes) - default 0
- `strSchedEn` - stream schedule enable (default false)
- `strSchStart` (minutes from midnight, 0..1439) - stream window start
- `strSchStop` (minutes from midnight, 0..1439) - stream window stop
- `deepSchSlp` - enable deep sleep outside schedule window (default false)

Thermal:
- `ohEnable` - default true
- `ohThresh` (C, step 5) - default 80
- `ohLatched` - persisted latch state
- `ohReason`, `ohStamp`, `ohTripC` - persisted info about the latest thermal shutdown

Apply changes via Web UI/API; `restartI2S()` is called on relevant updates.

### High-pass filter (HPF)

- Built-in 2nd-order high-pass filter to reduce low-frequency rumble.
- UI: Audio -> `High-pass` ON/OFF, `HPF Cutoff` (Hz).
- API:
  - Enable/disable: `GET /api/set?key=hp_enable&value=on|off`
  - Set cutoff: `GET /api/set?key=hp_cutoff&value=<Hz>`

### Stream schedule (time window)

- UI: Time & Network -> `Stream Schedule`, `Stream Start`, `Stream Stop`, `Schedule Status`.
- API:
  - Enable/disable: `GET /api/set?key=stream_sched&value=on|off`
  - Set start (minutes): `GET /api/set?key=stream_start_min&value=<0..1439>`
  - Set stop (minutes): `GET /api/set?key=stream_stop_min&value=<0..1439>`
  - Deep sleep outside window ON/OFF: `GET /api/set?key=deep_sleep_sched&value=on|off`
- `/api/status` includes:
  - `stream_schedule_enabled`
  - `stream_schedule_start_min`
  - `stream_schedule_stop_min`
  - `stream_schedule_allow_now`
  - `stream_schedule_time_valid`
  - `deep_sleep_sched_enabled`
  - `deep_sleep_status_code`
  - `deep_sleep_next_sec`

---

## Web UI & JSON API

- Status: IP, Wi-Fi RSSI, TX power, uptime, client, streaming, packet-rate.
- Time & Network: NTP sync state, last sync, time offset (hours), mDNS toggle, RTSP URLs (IP + mDNS),
  stream schedule (ON/OFF + start/stop + status), optional deep sleep outside schedule window (ON/OFF + status),
  Wi-Fi reset action, log download.
- Audio: edit values inline (Sample rate, Gain, Buffer). Latency and Profile are computed.
- Reliability: auto-recovery (auto/manual threshold mode), check interval.
- Thermal: enable/disable overheat protection, shutdown limit (30-95 C, step 5), status and last
  shutdown info (`/api/thermal`). The latch survives reboots and must be acknowledged in the UI.
- Wi-Fi: TX Power (dBm) editable inline.
- Actions: Server ON/OFF, Reset I2S, Reboot, Defaults (restores app settings and reboots).

The API mirrors the UI. Open DevTools -> Network to inspect endpoints and JSON.

---

## RTSP Details (From Code)

- DESCRIBE returns SDP with `a=rtpmap:96 L16/<sample-rate>/1` and `a=control:track1`.
- SETUP uses `RTP/AVP/TCP;unicast;interleaved=0-1` (server keeps a single client).
- PLAY starts streaming; TEARDOWN stops it.
- 30 s inactivity timeout when not streaming.
- RTP timestamp increases by the number of audio samples per packet.

---

## Diagnostics & Stability

- Wi-Fi: aim for RSSI > -75 dBm; consider fixed channel if possible.
- Buffers: increase above 512 in RF-noisy environments for smoother stream (adds latency).
- Auto-recovery: pipeline restarts if `packet-rate < minRate`.
- Thermal protection: when die temp >= limit (default 80 C), streaming stops and RTSP server is
  disabled. The latch stays active across reboots until you acknowledge it in the UI.
- Logs: use the ring buffer in the Web UI for drops/reconnects.
- CPU: default 160 MHz for thermal/perf balance (adjustable in Advanced settings).

---

## Security

- Keep the device on a trusted LAN; do not expose HTTP/RTSP to the internet.
- Protect OTA with a password if you enable it.

---

## Limitations

- Single RTSP client at a time.
- No global authentication on Web UI/API by default.

## Credits

- Author: **@Sukecz**
