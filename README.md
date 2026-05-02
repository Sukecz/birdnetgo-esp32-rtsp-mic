<p align="center">
  <img src="birdlogo.png" alt="ESP32 RTSP Mic for BirdNET-Go" width="260" />
</p>

# ESP32 RTSP Mic for BirdNET-Go

ESP32-C6 + I2S MEMS microphone streamer that exposes a **mono 16-bit PCM** audio stream over
**RTSP**, designed as a simple network mic for **BirdNET-Go**.

- Latest firmware: **v1.7.0** (2026-04-29)
- Target firmware: `esp32_rtsp_mic_birdnetgo` (Web UI + JSON API)
- Changelog: `esp32_rtsp_mic_birdnetgo/CHANGELOG.md`
- License: MIT (`LICENSE`)
- One-click web flasher (recommended): **https://esp32mic.msmeteo.cz**
  (Chrome/Edge desktop, USB-C *data* cable)

## Quick Start (EN)

1. Open **https://esp32mic.msmeteo.cz**.
2. Click **Flash**, select the USB JTAG/serial device, wait for reboot.
3. On first boot the device starts AP **ESP32-RTSP-Mic-AP** (open).
   Connect and finish Wi-Fi setup at `192.168.4.1` (captive portal).
4. Open the Web UI: `http://<device-ip>/` (port **80**).
5. RTSP stream (BirdNET-Go/VLC/ffplay):
   `rtsp://<device-ip>:8554/audio` (or `rtsp://<device-hostname>.local:8554/audio` if mDNS is enabled).

## Wiring (XIAO ESP32-C6 + ICS-43434 / INMP441)

![Wiring / pinout](connection.png)

| ICS-43434 signal | XIAO ESP32-C6 GPIO | Notes |
|---|:--:|---|
| **BCLK / SCK** | **21** | I2S bit clock |
| **LRCLK / WS** | **1** | I2S word select |
| **SD (DOUT)** | **2** | I2S data out from mic |
| **VDD** | 3V3 | Power |
| **GND** | GND | Ground |

INMP441 note:
- INMP441 has also been reported to work without firmware changes when wired to the same I2S pins
  (`SCK` -> GPIO21, `WS` -> GPIO1, `SD` -> GPIO2). If your breakout exposes an `L/R` or `SEL`
  pin, set it to the left channel (typically GND), because the firmware reads the left I2S channel.
  See GitHub discussion #25: https://github.com/Sukecz/birdnetgo-esp32-rtsp-mic/discussions/25

Tips:
- Keep I2S wires short; for longer runs use shielded cable to reduce EMI.
- Some XIAO ESP32-C6 revisions use an RF switch (GPIO3/GPIO14). If you use a different board or
  internal antenna only, you may need to comment out the antenna GPIO block in `setup()`.

## Test The RTSP Audio

- VLC: *Media* → *Open Network Stream* → paste the RTSP URL.
- ffplay:
  `ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/audio`
- ffprobe:
  `ffprobe -rtsp_transport tcp rtsp://<device-ip>:8554/audio`

If VLC/ffplay works, BirdNET-Go will typically work too (just use the same RTSP URL as your input).

## Highlights (v1.6.0)

- Web UI (English) on port **80** with live status, logs, and controls
- JSON API for automation
- MQTT + Home Assistant Discovery: richer diagnostics entities (boot reason/counter, Wi-Fi reconnect count, stream uptime/client count, firmware build)
- MQTT publish interval configurable in UI/API (default `60 s`, range `10..3600`) with immediate event-driven state publish
- Stream schedule (start/stop local time) with overnight window support (for example `22:00-06:00`)
- Fail-open schedule policy when time is unavailable (stream stays allowed instead of blocking)
- Schedule edge-case rule: `Start == Stop` is an explicit empty window (stream blocked always)
- Time & Network UI extended with stream schedule controls, status, and help tooltips
- Time & Network UI shows current Local time, UTC time, and applied time offset
- Optional deep sleep outside the stream schedule window (conservative mode + double-confirm enable in UI)
- Deep sleep safety policy: never sleeps when time is unsynced, during startup grace, or with active stream/client
- Max one deep-sleep cycle is 8 hours (for lower wake-up overhead / better solar use)
- Deep sleep wakes with a 5-minute guard before stream window to reduce RTC-drift edge issues
- After deep-sleep timer wake, logs include retained sleep snapshot (cycle, planned sleep, entered time, schedule, offset)
- Logs panel keeps manual scroll position when reading older logs (auto-follows only near bottom)
- Mobile UI: top header/RTSP links now wrap correctly (no horizontal overflow scrollbar)
- Auto-recovery (manual/auto packet-rate thresholds)
- Scheduled reset, CPU frequency control
- Thermal protection with latch + acknowledge
- High-pass filter (HPF) configurable (reduce rumble)
- RTSP keep-alive (`GET_PARAMETER`), single client

Web UI screenshot:

![Web UI](webui.png)

## Recommended Hardware (TL;DR)

| Part | Qty | Notes | Link |
|---|---:|---|---|
| Seeed Studio XIAO ESP32-C6 | 1 | Target board (tested) | [AliExpress](https://www.aliexpress.com/item/1005007341738903.html) |
| MEMS I2S microphone **ICS-43434** | 1 | Supported/tested reference mic | [AliExpress](https://www.aliexpress.com/item/1005008956861273.html) |
| MEMS I2S microphone **INMP441** | 1 | Reported compatible with the same I2S wiring | - |
| Shielded cable (6 core) | optional | Helps reduce EMI on mic runs | [AliExpress](https://www.aliexpress.com/item/1005002586286399.html) |
| 220 V -> 5 V power supply | 1 | >= 1 A recommended for stability | [AliExpress](https://www.aliexpress.com/item/1005002624537795.html) |
| 2.4 GHz antenna (IPEX/U.FL) | optional | If your board/revision uses external antenna | [AliExpress](https://www.aliexpress.com/item/1005008490414283.html) |

Notes:
- Links are provided for convenience and may change over time. Always verify the exact part number
  (for example **ICS-43434**) in the listing before buying.

## Tips & Best Practices

- RTSP is **single-client** (only one connection at a time).
- Wi-Fi: aim for RSSI > -75 dBm; try buffer >= 512 for stability.
- Multiple devices: each device uses a unique default mDNS/OTA hostname like `esp32mic-a1b2c3`.
- Placement: keep the mic away from fans/EMI; shielded cable helps for longer runs.
- Security: keep the device on a trusted LAN; do not expose HTTP/RTSP to the public internet.

### RF Noise / Wi-Fi TX Power

If the audio sounds noisy or distorted, try lowering **Wi-Fi TX Power** in the Web UI.
Several users reported a large audio-quality improvement after reducing TX power when the access
point is nearby.

Suggested test:
- Set Wi-Fi TX Power to about **11 dBm**.
- Watch RSSI and stream stability for a few minutes.
- If packets drop or RSSI is weak, raise TX power step by step.

For enclosures, a metal box can help shield the mic/I2S wiring, but keep the Wi-Fi antenna outside
the metal enclosure. Otherwise the box will also shield Wi-Fi and may make connectivity worse.

### High-Pass Filter (Reduce Low-Frequency Rumble)

- Default: ON at 500 Hz (since v1.4.0).
- Typical cutoff range: 300-800 Hz depending on your environment.
- UI: Web UI -> Audio -> `High-pass` + `HPF Cutoff`.
- API:
  - Enable/disable: `POST /api/set` with body `key=hp_enable&value=on|off`
  - Set cutoff (Hz): `POST /api/set` with body `key=hp_cutoff&value=600`
  - For mutating calls, send header `X-ESP32MIC-CSRF: 1` (used by Web UI).

## Compatibility

- **Target board:** ESP32-C6 (tested with Seeed XIAO ESP32-C6).
- Other ESP32 variants may work with pin/I2S tweaks.
- **ICS-43434** is the supported/tested reference mic.
- **INMP441** has been reported by a user to work without firmware changes using the same I2S pins
  (GitHub discussion #25).
- Other I2S mics may be possible with matching I2S format/pin settings.

## More Docs (Build, API, Internals)

See `esp32_rtsp_mic_birdnetgo/README.md`.

## License

This project is released under the MIT License. See `LICENSE`.
