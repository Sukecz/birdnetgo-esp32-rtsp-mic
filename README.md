# ESP32 RTSP Mic for BirdNET-Go

This repository contains an ESP32-based I²S microphone streamer for BirdNET-Go. It runs on **Seeed XIAO ESP32-C6** with an **ICS-43434** digital microphone and exposes a mono 16-bit PCM stream over RTSP.

- Latest version: `esp32_rtsp_mic_birdnetgo` (Web UI + JSON API)

Key features (v1.2.0):
- Web UI on port 80 (English/Czech) with live status, controls and settings
- JSON API for automation/integration
- Auto-recovery when packet rate degrades; auto/manual threshold mode
- Scheduled reset (ON/OFF + hours), CPU frequency setting
- OTA update support and WiFi Manager onboarding
- Built-in high-pass filter (configurable) to cut low-frequency rumble (traffic, wind)
- RTSP keep-alive support (GET_PARAMETER)
- Web UI level meter with clipping warning and guidance (EN/CZ)

**Stream URL:** `rtsp://<device-ip>:8554/audio` (PCM L16, mono)

---

## Recommended hardware (TL;DR)

| Part | Qty | Notes | Link |
|---|---:|---|---|
| Seeed Studio XIAO ESP32-C6 | 1 | Target board (tested) | [AliExpress](https://www.aliexpress.com/item/1005007341738903.html) |
| MEMS I²S microphone **ICS-43434** | 1 | Digital I²S mic used by this project | [AliExpress](https://www.aliexpress.com/item/1005008956861273.html) |
| Shielded cable (6 core) | optional | Helps reduce EMI on mic runs | [AliExpress](https://www.aliexpress.com/item/1005002586286399.html) |
| 220 V → 5 V power supply | 1 | ≥1 A recommended for stability | [AliExpress](https://www.aliexpress.com/item/1005002624537795.html) |
| 2.4 GHz antenna (IPEX/U.FL) | optional | If your board/revision uses external antenna | [AliExpress](https://www.aliexpress.com/item/1005008490414283.html) |

> **Sourcing note:** Links are provided for convenience and may change over time. Always verify the exact part (e.g., **ICS-43434**) in the listing before buying.

---

## Getting started

- Open **`esp32_rtsp_mic_birdnetgo/README.md`** for hardware pinout, build instructions and full API reference.
- Flash the firmware for **ESP32-C6** (Arduino / PlatformIO).
- On first boot the device exposes a Wi-Fi AP for onboarding (WiFi Manager).  
- Access the Web UI on port 80 and the RTSP stream at `rtsp://<device-ip>:8554/audio`.

---

## Compatibility

- **Target board:** ESP32-C6 (tested with Seeed XIAO ESP32-C6).  
- Other ESP32 variants may work with minor pin changes and I²S config tweaks.
- Other I²S mics (e.g., INMP441) may be possible with configuration changes, but **ICS-43434** is the supported/tested reference.

---

## Tips & best practices

- **Wi-Fi stability:** Aim for RSSI better than ~-75 dBm; set audio buffer ≥ 512 for smoother streaming.
- **Placement:** Keep the mic away from fans and vibrating surfaces; use shielded cable for longer runs.
- **Defaults (v1.2.0):** 48 kHz, gain 1.2, buffer 1024, shift 12, HPF ON (500 Hz), CPU 160 MHz.
- **Security:** The Web UI is intended for trusted LANs. Consider enabling OTA password in code and avoid exposing the device to the open internet.

### High-pass filter (reduce low-frequency rumble)

- Purpose: Attenuate low-frequency noise (distant road traffic, wind buffeting, handling noise) that dominates the bottom of the spectrogram and masks bird vocalizations.
- What it is: A 2nd‑order high‑pass biquad (≈12 dB/octave) running in real time on the ESP32. Neutral when OFF. Very low CPU overhead. Default ON at 500 Hz (adjust in UI).

How to use
- Web UI → Audio: set `High-pass` to ON and choose `HPF Cutoff`.
- Recommended cutoff: 300–800 Hz depending on your site.
  - 300–400 Hz: gentle cleanup, preserves ambience and low calls.
  - 500–700 Hz: strong reduction of distant road rumble (good default to try).
  - 800+ Hz: maximum suppression, may reduce low‑pitched species.
- API control:
  - Enable/disable: `GET /api/set?key=hp_enable&value=on|off`
  - Set cutoff (Hz): `GET /api/set?key=hp_cutoff&value=600`

Notes and tips
- Trade‑off: Higher cutoff cleans more rumble but may suppress low‑frequency birds (e.g., pigeons, woodpecker drumming). Adjust while watching the spectrogram and listening.
- Gain: With default I²S shift, a gain of 1.0× is neutral; increase carefully to avoid clipping.

---

## Notes

- For stable streaming, good Wi-Fi RSSI (>-75 dBm) and buffer ≥512 are recommended.
- Auto-recovery restarts the audio pipeline if packet rate degrades.

---

## Roadmap / nice-to-have

- Datasheet links and alternative vendors (EU/CZ/US) in a dedicated `docs/hardware.md`
- Simple wiring diagram and enclosure suggestions
- Tested-hardware matrix (board + mic combos) with firmware versions
