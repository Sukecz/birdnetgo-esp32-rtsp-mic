ESP32 RTSP Mic for BirdNET-Go (v1.0.0)

Overview

- Purpose: Seeed XIAO ESP32‑C6 + ICS‑43434 I2S microphone streaming mono 16‑bit PCM audio over RTSP to BirdNET-Go.
- Initial release v1.0.0: clean Web UI (English/Czech), JSON API endpoints, in-memory logs, performance diagnostics, OTA and WiFi Manager.
- Default audio: 48 kHz, gain 0.8, buffer 1024 samples, I2S shift 12 bits.

Project Structure

- `esp32_rtsp_mic_birdnetgo.ino`: Core app (I2S + RTSP, WiFi/OTA, performance, recovery).
- `WebUI.h` / `WebUI.cpp`: Web server (port 80), bilingual UI, and JSON API.

Hardware

- Board: Seeed XIAO ESP32-C6 (ESP32‑C6). CPU 120–160 MHz recommended.
- Mic: ICS-43434 (I2S, mono). Typical pins:
  - `I2S_BCLK_PIN (BCLK)`: 21
  - `I2S_LRCLK_PIN (WS/LRCLK)`: 1
  - `I2S_DOUT_PIN (SD)`: 2
- Antenna select (XIAO ESP32-C6 only): GPIO3 LOW + GPIO14 HIGH selects external antenna.

Security & OTA

- OTA password is optional. By default, it is not set.
- To enable protection, edit `esp32_rtsp_mic_birdnetgo.ino` and set:
  - `#define OTA_PASSWORD "1234"` (use your own strong password)
- If undefined, OTA runs without a password (suitable for trusted LAN only).

Build & Flash

1) Arduino IDE (2.x) with ESP32 board support (ESP32-C6).
2) Libraries: WiFiManager, ArduinoOTA (part of ESP32 core), Preferences (ESP32), WebServer (ESP32 core).
3) Open `esp32_rtsp_mic_birdnetgo/esp32_rtsp_mic_birdnetgo.ino` and flash.
4) On first boot, device starts AP `ESP32-RTSP-Mic-AP` for WiFi config.

Web UI

- URL: `http://<device-ip>/`
- Language: English/Čeština (toggle in top-right; stored in browser).
- Panels:
  - Status: WiFi RSSI, TX power, heap, uptime, RTSP server status, client, streaming state, packet rate, last connect/play times.
  - Audio: sample rate, gain, buffer, I2S shift, computed latency, profile hint.
  - Performance: auto-recovery toggle, restart threshold (pkt/s), check interval (min).
  - WiFi: TX power control (dBm).
  - Thermal: CPU temperature and max observed.
  - Logs: recent events buffered in RAM.

JSON API

- `GET /api/status` — core runtime status.
- `GET /api/audio_status` — audio params and derived latency/profile.
- `GET /api/perf_status` — performance monitor settings.
- `GET /api/thermal` — temperature and CPU MHz.
- `GET /api/logs` — last log lines (text/plain).
- Actions:
  - `GET /api/action/start`
  - `GET /api/action/stop`
  - `GET /api/action/disconnect`
  - `GET /api/action/server_start`
  - `GET /api/action/server_stop`
  - `GET /api/action/reset_i2s`
- Settings: `GET /api/set?key=...&value=...`
  - `gain` (0.1–100.0)
  - `rate` (8000–96000 Hz)
  - `buffer` (256–8192)
  - `shift` (0–24 bits)
  - `wifi_tx` (−1.0 to 19.5 dBm)
  - `auto_recovery` (`on`/`off`)
  - `min_rate` (5–200 pkt/s)
  - `check_interval` (1–60 minutes)
  - `sched_reset` (`on`/`off`)
  - `reset_hours` (1–168 hours)
  - `cpu_freq` (40–160 MHz)

BirdNET-Go Integration

- Audio format: RTSP over TCP, payload type 96, L16/mono at selected sample rate.
- URL: `rtsp://<device-ip>:8554/audio`
- Recommended BirdNET-Go settings: match device sample rate (48 kHz default), mono PCM L16.

Recommended Profiles

- Stable Streaming (lower CPU): buffer 1024, 48 kHz, gain ~0.8–15.0 depending on environment.
- Balanced: buffer 512 (default), 48 kHz.
- Ultra-low latency: buffer 256 (higher CPU, possible dropouts).

Performance & Recovery

- Auto-recovery: restarts I2S if packet rate drops below threshold (default 50 pkt/s).
- Check interval default: 15 minutes.
- Diagnostics include min free heap, max/min packet rate window, last I2S reset and last client connect/play.
- Thermal note: if >80°C observed, consider lowering CPU frequency or adding cooling.

Security

- OTA password is set in `esp32_rtsp_mic_birdnetgo.ino` (`OTA_PASSWORD`). Change before deploying.
- Web UI has no authentication (LAN usage assumed). If required, place behind a secured network.

Troubleshooting

- No RTSP client: ensure BirdNET-Go can reach device IP and port 8554.
- Dropouts: increase buffer size (e.g., 1024), check WiFi RSSI (>-75 dBm recommended), reduce TX power if overheating.
- Clipping: reduce gain or increase I2S shift.
- Heat: reduce CPU frequency via Web UI (e.g., 120 MHz).

License

- Provide your preferred license file at repository root if needed.

GitHub Topics (add in repo settings)

- birdnet-go, birdnet, esp32, esp32-c6, seeed-xiao, ics43434, microphone, i2s, rtsp, audio-streaming, arduino, ota, webui
