<p align="center">
  <img src="../assets/birdlogo.png" alt="ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi" width="240" />
</p>

# birdnet-esp32-rtsp-mic Firmware

Arduino firmware for Seeed XIAO ESP32 I2S microphones that serve **mono 16-bit PCM/L16** audio over
**RTSP** for **BirdNET-Go** and **BirdNET-Pi**. It also provides a Web UI, JSON API, MQTT telemetry,
and Home Assistant MQTT Discovery.

- Latest firmware: **v1.20** (2026-07-21)
- Build targets: Seeed Studio **XIAO ESP32-C3**, **XIAO ESP32-S3**, **XIAO ESP32-C5**, **XIAO ESP32-C6**
- Runtime-tested board: Seeed Studio **XIAO ESP32-C6**
- Reference microphone: **ICS-43434**; **INMP441** has been reported compatible with the same wiring
- User-facing overview and wiring: `../README.md`
- Changelog: `CHANGELOG.md`
- Web flasher: **https://esp32mic.msmeteo.cz**
- Manual OTA firmware: `../manual-ota-firmware/firmware-app-<board>.bin` (`firmware-app.bin` remains the C6 alias)
- License: MIT (`../LICENSE`)

## Important URLs

Web UI:

```text
http://<device-ip>/
```

RTSP streams:

```text
rtsp://<device-ip>:8554/audio1    Stream 1
rtsp://<device-ip>:8554/audio2    Stream 2
```

mDNS variants, if enabled and supported by your network:

```text
rtsp://<device-hostname>.local:8554/audio1
rtsp://<device-hostname>.local:8554/audio2
```

The API and Web UI publish `/audio1` and `/audio2`. Use `/audio1` in new configurations. `/audio`
remains available only as a compatibility alias for stream 1.

## First Boot

1. Flash with the web flasher or build manually.
2. The device starts WiFiManager AP **ESP32-RTSP-Mic-AP**.
3. Connect to the AP and open `192.168.4.1` if the captive portal does not open automatically.
4. Save Wi-Fi credentials.
5. After reboot, open `http://<device-ip>/`.

Default hostname is unique per device, for example `esp32mic-a1b2c3`.

## What's New In v1.20

- Version-only test build for validating the improved OTA installation progress and automatic
  browser reconnection workflow from v1.18. There are no functional changes compared with v1.18.

## What's New In v1.18

- OTA installation immediately shows a blocking progress state, protects against accidentally
  leaving the page, and reconnects automatically after the device reboots.
- Client-requested RTSP TEARDOWN events now log session duration, packet and RTP timing, transport,
  client address, CSeq, Wi-Fi RSSI, and User-Agent for easier correlation with BirdNET-Go.

## What's New In v1.17

- Version-only test build for validating the complete Web UI automatic OTA workflow from v1.16.
  There are no functional firmware changes compared with v1.16.

## What's New In v1.16

- Audio configuration changes stop the producer before publishing a new buffer size, preventing the
  producer from reading a larger block into the previous smaller allocation.
- Firmware availability checks run in a background task, and MQTT reconnect attempts are deferred
  while RTSP is streaming, so unavailable internet or MQTT services do not block the stream loop.
- HPF cutoff values are kept below 45% of the active sample rate, preventing repeated coefficient
  resets for invalid rate/cutoff combinations.
- Persisted settings are validated at boot and invalid values are replaced with safe defaults instead
  of risking allocation failures or restart loops. Factory defaults now reliably re-enable time sync.
- Default gain is 1.5; the default packet buffer remains 512 samples.

## What's New In v1.15

- Automatic OTA now checks `ota-version.txt` and compares `major.minor` versions before downloading.
  The install endpoint blocks equal, older, malformed, or unavailable versions, so an unnecessary
  firmware download cannot interrupt the stream.
- The main Web UI shows a prominent update banner only when a newer version is confirmed. The
  firmware page separately reports update available, up to date, or check failed.
- Offline operation remains supported: a failed check is cached for one hour, the main UI makes no
  automatic retry in the same page session, and manual app-only firmware upload remains available.

## What's New In v1.14

- Fixed long-running packet-rate calculations to use 64-bit arithmetic. The previous 32-bit
  multiplication overflowed after about 12.7 hours at 48 kHz/512 samples and caused false automatic
  I2S recovery.
- Increased the internal audio ring-buffer reserve to at least 32 KiB for normal packet sizes while
  retaining the 512-sample RTP/UDP packet default.
- Extended the TCP write tolerance from 30 ms to 100 ms and added failure diagnostics for partial
  progress, elapsed time, available TCP space, ring-buffer use, connection state, and Wi-Fi RSSI.

## What's New In v1.13

- Migrated microphone capture from the deprecated legacy I2S API to the current channel-based
  `i2s_std` API without changing the existing BCLK/WS/SD wiring or adding an MCLK output.
- RTSP-over-TCP now combines the interleaved framing header, RTP header, and audio payload before
  writing, reducing calls into the TCP stack.
- Web UI request handling is capped at 50 Hz so an open browser tab does not monopolize the main
  loop while audio is streaming.
- The default audio packet buffer remains **512 samples** for BirdNET-Pi/UDP compatibility.

## What's New In v1.12

- Automatic OTA now uses the board-specific stable latest alias (`firmware-app-c3.bin`,
  `firmware-app-s3.bin`, `firmware-app-c5.bin`, or `firmware-app-c6.bin`). It no longer builds a URL
  from the currently installed version, which prevented older devices from discovering a release.
- The automatic OTA URL is compiled from the board profile, is not editable in the Web UI, and must
  exactly match that board. Unsupported generic builds no longer fall back to the C6 image.
- OTA pull validates HTTP status, content type, content length, download completeness, app partition
  capacity, and premature connection closure before accepting the update.
- Release validation checks version format, board mappings, stable/versioned/manual binary equality,
  C6 compatibility aliases, and public firmware checksums.

## What's New In v1.11

- Fixed I2S pin selection for XIAO ESP32-C3/S3/C5 builds. Arduino exposes `D1`/`D2`/`D3` as C++
  constants, so the previous preprocessor test did not detect them and incorrectly selected the C6
  fallback GPIOs (`21/1/2`) for every board.
- I2S startup logs now include the active BCLK/WS/SD GPIO numbers for wiring diagnostics.

## What's New In v1.10.1

- OTA update endpoints now require the same mutation header as other state-changing Web UI/API calls.
- Automatic OTA URLs were generated from the compiled firmware version. This behavior was replaced
  in v1.12 because it pinned devices to their already-installed release.
- OTA rejects oversized or merged USB firmware images, including 4 MB and 8 MB full-flash files.
- GitHub Actions compile-checks all supported XIAO ESP32-C3/S3/C5/C6 targets.

## What's New In v1.10.0

- Added board profiles for Seeed Studio XIAO ESP32-C3, XIAO ESP32-S3, XIAO ESP32-C5, and XIAO ESP32-C6.
- I2S wiring now uses the same physical XIAO pin labels on supported boards: `D3` for BCLK, `D1`
  for LRCLK/WS, and `D2` for data.
- XIAO ESP32-C6 keeps firmware-controlled external antenna selection through GPIO3/GPIO14.
- XIAO ESP32-C3, XIAO ESP32-S3, and XIAO ESP32-C5 builds do not touch antenna-control GPIOs.
- `/api/status`, MQTT device metadata, and the OTA page expose the compiled board profile.
- Web flasher artifacts now include separate C3/S3/C5/C6 images and a manifest that lets ESP Web
  Tools select firmware by connected chip family.

## What's New In v1.9.3

- Default buffer is now **512 samples** after BirdNET-Pi UDP testing showed stutter with 1024-sample
  packets.
- BirdNET-Pi UDP compatibility is improved by handling the advertised RTCP port and adding standard
  RTP metadata expected by ffmpeg-based clients.
- UDP was validated with a clean Nachtzuster/BirdNET-Pi install using `/audio2` in BirdNET-Pi mode.
- Sample rate settings now accept **8,000-192,000 Hz** in both the Web UI and API.
- Audio settings are saved only after the I2S pipeline restarts successfully; unsupported settings
  roll back immediately instead of silently reverting later.
- Web UI dirty-field highlighting now clears reliably after successful saves and stays visible when
  a save is rejected.

## Hardware

### I2S Wiring

![Wiring / pinout](../assets/connection.png)

Use the same physical XIAO pin labels on every supported board. The underlying GPIO numbers differ
by chip.

| Mic signal | XIAO pin label | C3 GPIO | S3 GPIO | C5 GPIO | C6 GPIO | Code define |
|---:|:--:|:--:|:--:|:--:|:--:|---|
| **BCLK / SCK** | **D3** | 5 | 4 | 7 | 21 | `I2S_BCLK_PIN` |
| **LRCLK / WS** | **D1** | 3 | 2 | 0 | 1 | `I2S_LRCLK_PIN` |
| **SD / DOUT** | **D2** | 4 | 3 | 25 | 2 | `I2S_DOUT_PIN` |
| **VDD** | 3V3 | - | - | - | - | Power |
| **GND** | GND | - | - | - | - | Ground |

The firmware configures I2S as master/RX, reads the left channel, then shifts/scales samples to
16-bit PCM. If using INMP441, set `L/R` or `SEL` to the left channel, usually GND.

### XIAO Antenna Paths

The firmware selects the external antenna path on XIAO ESP32-C6:

```text
GPIO3  -> LOW
GPIO14 -> HIGH
```

Use an external 2.4 GHz antenna for reliable streaming. Weak Wi-Fi increases retries, heat, and the
chance of audio dropouts. XIAO ESP32-C3 and XIAO ESP32-S3 expose a U.FL antenna connector without a
documented firmware GPIO switch. XIAO ESP32-C5 has a dedicated Wi-Fi/BT antenna connector; attach
the included antenna before use. If your C6 hardware uses the internal antenna path, adjust the
GPIO3/GPIO14 block in `setup()`.

## Runtime Defaults

- Sample rate: 48 kHz
- Audio format: mono 16-bit PCM/L16
- Gain: 1.5
- Buffer: 512 samples
- I2S shift: 12 bits
- High-pass filter: ON, 500 Hz
- Wi-Fi TX power: about 19.5 dBm
- CPU: 160 MHz
- Thermal shutdown: 80 C, protection ON
- Max RTSP clients: 2
- mDNS: ON
- Time sync: ON
- XIAO ESP32-C6 antenna path: external antenna ON
- XIAO ESP32-C3/S3/C5 antenna path: no firmware GPIO switching
- OTA password: none in the default public build

The sample-rate setting accepts 8,000-192,000 Hz. 192 kHz is allowed by the UI/API for users who
want to test ultrasonic-capable microphones, but bandwidth, CPU load, I2S stability, and BirdNET
backend support should be verified on the target setup.

## Web UI

The Web UI runs on port **80** and includes:

- Status: IP, RSSI, uptime, heap, server state, stream states, packet rates.
- Streams: URLs for `/audio1` and `/audio2`, enable/disable, max clients, BirdNET target.
- Audio: sample rate, gain, buffer size, I2S shift, high-pass filter, signal level.
- Audio API diagnostics: producer state, ring-buffer capacity/chunks/drops/flushes, I2S errors,
  and RTSP write stalls/timeouts.
- Time & Network: NTP state, time offset, mDNS, stream schedule, optional deep sleep, Wi-Fi actions.
- Reliability: auto-recovery, threshold mode, check interval, scheduled reset.
- Thermal: current/peak temperature, shutdown limit, protection latch, acknowledgement.
- MQTT & Home Assistant: broker settings, publish interval, discovery republish.
- Firmware update: open `/ota` or click **Firmware update** in the Web UI.

## Firmware Update Without USB

Open:

```text
http://<device-ip>/ota
```

You have two choices:

1. **Automatic update**: use this when the device has internet access. The device downloads
   the board-specific app-only firmware from `esp32mic.msmeteo.cz` and installs it.
2. **Upload compiled file**: use this when the device has no internet access. Upload the app-only
   `.bin` file from your computer. The current release file is
   the matching app-only file from `../manual-ota-firmware/`.

Automatic update uses a board-specific stable plain HTTP URL, for example
`http://esp32mic.msmeteo.cz/firmware-app-c3.bin`. The build/deploy workflow replaces that alias with
the latest published app image on every release. The URL is fixed by the compiled board profile and
cannot be edited in the OTA page, preventing a C3/S3/C5/C6 device from pulling another board's image.
Unsupported generic builds must use manual OTA with a matching app-only file.

The server must provide these files over plain HTTP without redirects, with an octet-stream content
type and a valid content length. HTTPS/TLS does not fit in the tight XIAO ESP32-C3/C6 default app
partitions. `firmware-app.bin` remains a C6 compatibility alias for older firmware.

- Logs: ring buffer view and download.
- Actions: RTSP server ON/OFF, reset I2S, reconnect Wi-Fi, reboot, restore defaults.

## Stream Behavior

- Stream 1: `/audio1`; `/audio` is a compatibility alias for the same stream.
- Stream 2: `/audio2`.
- Each stream can be enabled or disabled.
- Each stream can target BirdNET-Go or BirdNET-Pi.
- BirdNET-Go target uses RTP over RTSP/TCP.
- BirdNET-Pi target uses RTP/UDP when the client provides UDP ports.
- RTSP keep-alive via `GET_PARAMETER` is supported.
- Inactive non-streaming sessions time out after about 30 seconds.

### Verify With ffmpeg Tools

```bash
ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/audio1
ffprobe -rtsp_transport tcp rtsp://<device-ip>:8554/audio1
```

For stream 2, replace `/audio1` with `/audio2`. If VLC/ffplay works, use the same URL in
BirdNET-Go or BirdNET-Pi.

## Network And Time

- Wi-Fi power save is disabled with `WiFi.setSleep(false)` for stable streaming.
- WiFiManager AP: `ESP32-RTSP-Mic-AP`.
- WiFiManager connect timeout: 60 s.
- WiFiManager portal timeout: 180 s.
- mDNS can be enabled/disabled in the Web UI.
- Hostname can be changed via API: `key=mdns_hostname&value=esp32mic-garden`.
- mDNS often fails on isolated/guest Wi-Fi or inside Docker containers; use device IP in those cases.
- NTP sync runs on boot, retries every hour until synced, then refreshes every 6 hours.
- If time is unavailable, logs fall back to uptime timestamps.

### Stream Schedule And Deep Sleep

Stream schedule is configured in Time & Network.

- Cross-midnight windows are supported, for example `22:00-06:00`.
- If time is invalid, schedule policy is fail-open: streaming stays allowed.
- If start and stop are equal, the window is explicitly empty and streaming is blocked.
- Optional deep sleep can run outside the stream window only when time is valid.
- Deep sleep is blocked during startup grace, with active clients, or without valid time.

API keys:

```text
stream_sched=on|off
stream_start_min=<0..1439>
stream_stop_min=<0..1439>
deep_sleep_sched=on|off
```

## JSON API

The API mirrors the Web UI. Inspect browser DevTools -> Network for exact calls.

Important read endpoints:

```text
GET /api/status
GET /api/audio_status
GET /api/perf_status
GET /api/thermal
GET /api/logs
```

Mutating calls use `POST` and require header `X-ESP32MIC-CSRF: 1`.

Common settings endpoint:

```text
POST /api/set
```

Examples of request bodies:

```text
key=stream1_enabled&value=on
key=stream2_enabled&value=off
key=max_clients&value=2
key=stream1_target&value=0
key=stream2_target&value=1
key=hp_enable&value=on
key=hp_cutoff&value=600
```

Target values:

```text
0 = BirdNET-Go
1 = BirdNET-Pi
```

`/api/status` includes stream URLs and state, including:

```text
stream1_url_ip
stream2_url_ip
stream1_url_mdns
stream2_url_mdns
stream1_enabled
stream2_enabled
stream1_target
stream2_target
s1_clients
s2_clients
s1_streaming
s2_streaming
s1_pkt_rate
s2_pkt_rate
max_clients
```

`/api/audio_status` includes audio pipeline diagnostics:

```text
producer_running
i2s_error_count
rb_capacity_bytes
rb_chunks
rb_drops
rb_flushes
rtsp_write_stalls
rtsp_write_timeouts
```

## MQTT And Home Assistant

MQTT settings are available in the Web UI and API.

- Telemetry topic: `<topic_prefix>/state`
- Availability topic: `<topic_prefix>/availability`
- RTSP server command: `<topic_prefix>/cmd/rtsp_server` with `ON` or `OFF`
- Reboot command: `<topic_prefix>/cmd/reboot` with `PRESS` or `REBOOT`
- Publish interval: default 60 s, range 10-3600 s
- Immediate publishes happen on important events such as stream start/stop and connection changes.

Home Assistant MQTT Discovery creates entities for runtime values such as RSSI, uptime, heap,
temperature, stream state, packet rate, client count, reboot reason, restart counter, firmware
version/build, and Wi-Fi reconnect count.

MQTT password is stored in device flash in plain text NVS.

## Build And Flash Manually

### Arduino IDE

1. Open `esp32-birdnet-mic/esp32-birdnet-mic.ino`.
2. Install an ESP32 Arduino core with support for the selected XIAO target.
3. Select the matching board, for example *XIAO_ESP32C3*, *XIAO_ESP32S3*, *XIAO_ESP32C5*, or
   *XIAO_ESP32C6*.
4. Compile and upload over USB.

The sketch folder contains `build_opt.h`. Keep it next to the `.ino`; Arduino IDE uses it
automatically to keep the tight XIAO ESP32-C3/C6 default 1.2 MB app partitions below the size limit.

### arduino-cli

```bash
arduino-cli compile --fqbn <BOARD_FQBN> esp32-birdnet-mic
arduino-cli upload -p <PORT> --fqbn <BOARD_FQBN> esp32-birdnet-mic
```

From outside the repository, use the full sketch path:

```bash
arduino-cli compile --fqbn <BOARD_FQBN> esp32-birdnet-mic
```

### PlatformIO

Typical targets are board-specific Arduino environments, for example `env:xiao_esp32c6`:

```bash
pio run -t upload
```

## Web UI Development

The Web UI source is `webui/index.html`. Firmware serves the compressed generated header
`WebUI_gz.h` from PROGMEM.

After editing the UI, regenerate the header:

```bash
./tools/gen_webui_gzip_header.sh
```

## Persisted Configuration

Most runtime settings are stored in NVS namespace `audio` through ESP32 Preferences.

Main keys:

```text
sampleRate       Audio sample rate
gainFactor      Audio gain
bufferSize       Samples per packet/buffer profile
shiftBits        I2S right shift before gain
hpEnable         High-pass enable
hpCutoff         High-pass cutoff Hz
wifiTxDbm        Wi-Fi TX power
mdnsEn           mDNS enable
timeSyncEn       NTP enable
timeOffset       Local offset in minutes
strSchedEn       Stream schedule enable
strSchStart      Stream window start minute
strSchStop       Stream window stop minute
deepSchSlp       Deep sleep outside schedule window
autoRecovery     Packet-rate recovery enable
thrAuto          Automatic threshold mode
minRate          Manual minimum packet rate
checkInterval    Recovery check interval in minutes
schedReset       Scheduled reset enable
resetHours       Scheduled reset interval
ohEnable         Thermal protection enable
ohThresh         Thermal shutdown threshold C
ohLatched        Persisted thermal latch
```

Apply changes through Web UI or API. Audio-related updates call `restartI2S()` when needed.
Current validation ranges include `sampleRate=8000..192000` and `bufferSize=256..8192`.

## RTSP Implementation Notes

- I2S capture and audio processing run in a producer task.
- RTSP output consumes processed PCM blocks from a FreeRTOS ring buffer and packetizes them as RTP.
- `DESCRIBE` returns SDP with `a=rtpmap:96 L16/<sample-rate>/1` and `a=control:track1`.
- Stream selection is path-based: `/audio1` and `/audio` select stream 1; `/audio2` selects stream 2.
- `SETUP` returns either `RTP/AVP/TCP;unicast;interleaved=0-1` or UDP ports, depending on target.
- `PLAY` starts RTP packet output for that session.
- `TEARDOWN` stops the session.
- RTP timestamp increments by the number of audio samples per packet.

## Stability Notes

- Aim for Wi-Fi RSSI better than about **-75 dBm**.
- Increase buffer size in RF-noisy environments; this adds latency but improves stability.
- If audio is noisy while Wi-Fi is otherwise stable, try lowering Wi-Fi TX power before changing audio settings.
- Keep I2S wires short and away from the ESP32 RF area.
- Use shielded cable for longer microphone runs.
- Thermal protection disables RTSP when the chip reaches the configured limit; the latch survives reboot until acknowledged in the Web UI.

## Security

- Keep the device on a trusted LAN.
- Do not expose HTTP, RTSP, or OTA to the public internet.
- The default public build has no OTA password.
- If you build your own firmware for untrusted networks, set an OTA password before publishing it.
- Mutating API endpoints require `POST` and `X-ESP32MIC-CSRF: 1`, but read endpoints are not globally authenticated by default.

## Known Limitations

- No TLS or built-in user authentication for the Web UI/API.
- mDNS depends on multicast support in your LAN and often does not work across VLANs, guest networks, or Docker bridge networks.
- The firmware is primarily runtime-tested on Seeed Studio XIAO ESP32-C6 with ICS-43434; C3/S3/C5
  builds are compile-verified in Arduino ESP32 core 3.3.8.

## Credits

- Author: **@Sukecz**

## License

This firmware is released under the MIT License. See `../LICENSE`.
