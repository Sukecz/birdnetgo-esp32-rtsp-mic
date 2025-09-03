# ESP32 RTSP Mic for BirdNET-Go

This repository contains an ESP32-based I²S microphone streamer for BirdNET-Go. It runs on **Seeed XIAO ESP32-C6** with an **ICS-43434** digital microphone and exposes a mono 16-bit PCM stream over RTSP.

- Latest version: `esp32_rtsp_mic_birdnetgo` (Web UI + JSON API)

Key features (v1.0.0):
- Web UI on port 80 (English/Čeština) with live status, controls and settings
- JSON API for automation/integration
- Auto-recovery when packet rate degrades
- OTA update support and WiFi Manager onboarding

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
- **Security:** The Web UI is intended for trusted LANs. Consider enabling OTA password in code and avoid exposing the device to the open internet.

---

## Notes

- For stable streaming, good Wi-Fi RSSI (>-75 dBm) and buffer ≥512 are recommended.
- Auto-recovery restarts the audio pipeline if packet rate degrades.

---

## Roadmap / nice-to-have

- Datasheet links and alternative vendors (EU/CZ/US) in a dedicated `docs/hardware.md`
- Simple wiring diagram and enclosure suggestions
- Tested-hardware matrix (board + mic combos) with firmware versions

