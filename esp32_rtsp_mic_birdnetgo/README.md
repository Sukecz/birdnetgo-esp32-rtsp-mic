# ESP32 RTSP Mic for BirdNET-Go — Internal README (EN)

An ESP32‑C6 + I²S digital microphone (ICS‑43434) streamer that exposes a **mono 16‑bit PCM** audio stream over **RTSP** for **BirdNET‑Go**. It includes a Web UI (EN/CZ), a compact JSON API, OTA hooks, and auto‑recovery.

---

## TL;DR

- **Web UI:** `http://<device-ip>/` (port **80**)  
- **RTSP audio:** `rtsp://<device-ip>:8554/audio` (**L16/PCM**, mono, **RTP over TCP**)  
- **Board:** Seeed Studio **XIAO ESP32‑C6** (tested)  
- **Mic:** **ICS‑43434** (I²S, mono)  
- **Defaults:** 48 kHz sample‑rate, gain 0.8, buffer 1024, Wi‑Fi TX ≈ 19.5 dBm, shiftBits 12
- **Onboarding:** WiFiManager AP **ESP32‑RTSP‑Mic‑AP** on first boot  
- **Clients:** One RTSP client at a time (single connection handled)

---

## Architecture (quick map)

- **MCU:** ESP32‑C6 (Seeed XIAO ESP32‑C6 reference)
- **Input:** I²S MEMS mic (ICS‑43434 reference)
- **Output:** RTSP server on **8554** → `audio` track, **L16/mono/16‑bit PCM**  
  RTP dynamic PT **96**, `rtpmap:96 L16/<sample-rate>/1`, transport **RTP/AVP/TCP;interleaved=0-1`
- **Control:** Web UI (EN/CZ) + JSON API (status, audio, perf/thermal, logs, actions, settings)
- **Reliability:** watchdogs + auto‑recovery when packet‑rate drops below threshold
- **OTA:** optional; protect with a password if enabled
- **Timeouts:** RTSP inactivity timeout ~30 s; Wi‑Fi health checks and reconnection

---

## Pinout & Wiring (from source)

### I²S (ICS‑43434 ↔ ESP32‑C6)
| ICS‑43434 Signal | ESP32‑C6 GPIO | Notes |
|---:|:--:|---|
| **BCLK / SCK** | **21** | `#define I2S_BCLK_PIN 21` |
| **LRCLK / WS** | **1**  | `#define I2S_LRCLK_PIN 1`  |
| **SD (DOUT)**  | **2**  | `#define I2S_DOUT_PIN 2`   |
| **VDD**        | 3V3    | Power |
| **GND**        | GND    | Ground |

- I²S mode: **Master / RX**, **32‑bit** samples read, **ONLY_LEFT** channel; software shifts & scales to 16‑bit PCM.
- DMA: 8 buffers, **buf_len = min(bufferSize, 512)** samples.

### Antenna control (XIAO ESP32‑C6)
- **GPIO3 → LOW** (RF switch control enabled)  
- **GPIO14 → HIGH** (select external antenna)

> Keep I²S lines short; use shielded cable for longer runs to reduce EMI.

---

## Build & Flash

### Arduino IDE (quick start)
1. Open **`esp32_rtsp_mic_birdnetgo/esp32_rtsp_mic_birdnetgo.ino`**.
2. Install **ESP32 Arduino core** with **ESP32‑C6** support.
3. Board: *Seeed XIAO ESP32‑C6* (or *ESP32‑C6 Dev Module*).
4. Compile & upload over USB‑UART.

### PlatformIO (VS Code)
- Framework **Arduino**, Platform **espressif32**, target **ESP32‑C6** (consider `env:xiao_esp32c6`).  
- Typical: `pio run -t upload`.  
- If you hit toolchain/core issues, update to a core with full C6 support.

---

## Configuration

### Compile‑time defaults
```c
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_GAIN_FACTOR 0.8f
#define DEFAULT_BUFFER_SIZE 1024
#define DEFAULT_WIFI_TX_DBM 19.5f
// I²S pins:
#define I2S_BCLK_PIN 21
#define I2S_LRCLK_PIN 1
#define I2S_DOUT_PIN 2
```
- **Shift bits (runtime):** `uint8_t i2sShiftBits = 12;` (global init)

### Runtime (persisted in NVS via Preferences)
Keys (namespace `"audio"`):  
- `sampleRate` (Hz) — default **48000**  
- `gainFactor` — default **0.8**  
- `bufferSize` (samples) — default **1024**  
- `shiftBits` — **12** on first boot (your new fallback)  
- `autoRecovery` — default **true**  
- `schedReset` — default **false**; `resetHours` default **24**  
- `minRate` — default **50** (pkt/s threshold)  
- `checkInterval` — default **15** (minutes)  
- `cpuFreq` — default **120** (MHz)  
- `wifiTxDbm` — default **19.5** (dBm)

> Apply changes via Web UI/API; `restartI2S()` is called on relevant updates.

---

## First Boot & Network

- Wi‑Fi power save **disabled** (`WiFi.setSleep(false)`) for stable streaming.  
- WiFiManager: AP **`ESP32-RTSP-Mic-AP`**, connect timeout **60 s**, portal timeout **180 s**.  
- After joining LAN, open **`http://<device-ip>/`**.  
- Verify RTSP in VLC/ffplay: **`rtsp://<device-ip>:8554/audio`** (TCP).

---

## Web UI & JSON API

- Status: IP, Wi‑Fi RSSI, TX power, uptime, client, streaming, packet‑rate.
- Audio: edit values inline (Sample rate, Gain, Buffer). Latency and Profile are computed.
- Reliability: Auto‑recovery (Auto/Manual threshold). Check interval configurable.
- Wi‑Fi: TX Power (dBm) editable inline.
- Actions: Server ON/OFF, Reset I2S, Reboot, Defaults (restores app settings and reboots).
- The API mirrors the UI — open **DevTools → Network** to inspect endpoints and JSON.

---

## RTSP details (from code)

- **DESCRIBE** returns SDP with `a=rtpmap:96 L16/<sample-rate>/1` and `a=control:track1`.
- **SETUP**: `RTP/AVP/TCP;unicast;interleaved=0-1` (server keeps a single client).  
- **PLAY** starts streaming; **TEARDOWN** stops it.  
- 30 s inactivity timeout when not streaming.  
- RTP timestamp increases by the number of audio samples per packet.

---

## Diagnostics & Stability

- **Wi‑Fi:** Aim for RSSI **> −75 dBm**; consider IoT VLAN and fixed channel if possible.
- **Buffers:** Increase above **512** in RF‑noisy environments for smoother stream (adds latency).  
- **Auto‑recovery:** pipeline restarts if `packet‑rate < minRate`.  
- **Logs:** check the ring buffer in the Web UI for drops/reconnects.
- **CPU:** default **120 MHz** for thermal/perf balance.

---

## Recommended hardware (quick list)

| Part | Qty | Notes |
|---|---:|---|
| Seeed Studio XIAO ESP32‑C6 | 1 | Target board (tested) |
| MEMS I²S microphone **ICS‑43434** | 1 | Digital I²S mic used by this project |
| Shielded cable (6 core) | optional | Reduces EMI on longer mic runs |
| 220 V → 5 V PSU (≥1 A) | 1 | Headroom for stability |
| 2.4 GHz antenna (IPEX/U.FL) | optional | For external‑antenna revisions |

> Verify the exact mic model (**ICS‑43434**) when sourcing.

---

## Security

- Keep the device on a **trusted LAN**; do **not** expose HTTP/RTSP to the internet.  
- Protect OTA with a password if you enable it.

---

## Limitations

- Single **RTSP client** at a time.  
- No global authentication on Web UI/API by default.

## Credits

- Author: **@Sukecz**
