# ESP32 RTSP Mic for BirdNET-Go

This repository contains an ESP32-based I2S microphone streamer for BirdNET-Go. It runs on Seeed XIAO ESP32‑C6 with an ICS‑43434 digital microphone and exposes a mono 16‑bit PCM stream over RTSP.

- Latest version: `esp32_rtsp_mic_birdnetgo` (Web UI + JSON API)

Key features (v1.0.0):
- Web UI on port 80 (English/Čeština) with live status, controls and settings
- JSON API for automation/integration
- Auto-recovery when packet rate degrades
- OTA update support and WiFi Manager onboarding

Getting started:
- Open `esp32_rtsp_mic_birdnetgo/README.md` for hardware pinout, build instructions and full API reference.
- Stream URL: `rtsp://<device-ip>:8554/audio`

Notes:
- Target board: ESP32-C6 (tested with Seeed XIAO ESP32-C6). Other ESP32 variants may work with minor pin changes.
- For stable streaming, good WiFi RSSI (>-75 dBm) and buffer ≥512 are recommended.
