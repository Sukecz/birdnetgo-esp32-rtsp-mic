## Web flasher

Static page for flashing the birdnet-esp32-rtsp-mic firmware (BirdNET-Go / BirdNET-Pi, Seeed XIAO
ESP32-C3/S3/C5/C6) directly from the browser with ESP Web Tools.

Current prepared images: **firmware 1.22** (2026-08-01).

ESP Web Tools automatically selects the `ESP32-C3`, `ESP32-S3`, `ESP32-C5`, or `ESP32-C6` manifest
entry based on the connected chip. It cannot distinguish between different boards with the same chip
family, so the manifest is intended for Seeed XIAO boards with these chips.

ESP Web Tools is bundled locally at `vendor/esp-web-tools/10.4.0/` with its Apache-2.0 license. The
public flasher therefore does not depend on an unversioned CDN module or external web fonts.

Note for 1.22: uses controlled non-blocking RTSP/TCP writes, flushes stale audio after recovered
transport stalls, lowers the default Wi-Fi TX power to 15 dBm, and filters isolated internal
temperature-sensor spikes before thermal shutdown.

Note for 1.21: outputs a 256-fs I2S MCLK on physical XIAO pin D7 for experimental PCM1808 ADC
setups, preserves the existing left-channel mono path for ICS-43434/INMP441, and improves MQTT
reconnect behavior during an active stream.

Note for 1.20: version-only OTA workflow test build with no functional changes compared with v1.18.

Note for 1.18: OTA installation now shows an immediate protected progress state and reconnects after
reboot. RTSP TEARDOWN logs now include detailed client and session context.

Note for 1.17: version-only test build for validating the complete device Web UI OTA workflow from
v1.16. There are no functional firmware changes compared with v1.16.

Note for 1.16: audio settings are applied transactionally, firmware checks no longer block the RTSP
loop, MQTT reconnect is deferred while streaming, HPF/NVS validation is hardened, default gain is
1.5, and the default buffer remains 512 samples.

Note for 1.15: automatic OTA compares the published and installed versions before downloading,
shows a prominent update notice in the device UI, and handles unavailable internet access without
repeated checks while keeping manual app-only upload available.

Note for 1.14: fixes false long-uptime performance recovery caused by 32-bit packet-rate overflow,
increases the internal ring-buffer reserve without changing the 512-sample RTP/UDP packet default,
and improves TCP backpressure tolerance and failure diagnostics.

Note for 1.13: microphone capture uses the current channel-based `i2s_std` API, RTSP/TCP combines
each interleaved RTP packet before writing, and Web UI handling is capped at 50 Hz. The existing
BCLK/WS/SD wiring and 512-sample default remain unchanged; MCLK is not enabled.

Note for 1.12: automatic OTA uses the board-specific stable `firmware-app-<board>.bin` alias, which
is replaced on every deployment. The device no longer pins the download URL to its currently
installed version, accepts a URL for another board, or falls back to C6 for an unsupported profile.
The OTA download also rejects invalid content types, missing sizes, short bodies, and disconnects.

Note for 1.11: fixes C3/S3/C5 I2S GPIO selection. Versions 1.10.0 and 1.10.1 incorrectly used
the C6 GPIO mapping (`21/1/2`) in every board image because Arduino pin labels were tested as
preprocessor macros instead of C++ constants. The C6 image was not affected.

Note for 1.10.1: the images include XIAO ESP32-C3, XIAO ESP32-S3, XIAO ESP32-C5, and
XIAO ESP32-C6 builds. OTA update endpoints require the Web UI mutation header and reject merged USB
firmware images.

Note for 1.10.0: the images add XIAO ESP32-C3, XIAO ESP32-S3, and XIAO ESP32-C5 alongside the
original XIAO ESP32-C6. The physical microphone wiring is the same by XIAO pin labels
`D3`/`D1`/`D2`, but the GPIO numbers differ between chips.

Note for 1.9.3: the image includes added RTSP/UDP compatibility for BirdNET-Pi
(`RTP-Info`, `source`/`ssrc`, and RTCP port handling). It was verified against a clean
`Nachtzuster/BirdNET-Pi` installation with the `/audio2` stream in BirdNET-Pi/UDP mode.

Default public builds:
- include separate firmware for XIAO ESP32-C3, XIAO ESP32-S3, XIAO ESP32-C5, and XIAO ESP32-C6,
- enable the external antenna on the C6 build through the GPIO3/GPIO14 RF switch,
- do not use firmware GPIO antenna switching on the C3/S3/C5 builds,
- do not have an OTA password set,
- belong only on a trusted local network.

Firmware 1.9.2 and newer includes an Arduino IDE build-size fix: the
`esp32-birdnet-mic/build_opt.h` file disables unused C++ exception/unwind metadata and keeps the
tight default 1.2 MB app partition on XIAO ESP32-C3/C6 below the limit without removing features.

The firmware is intended for the **ICS-43434** I2S microphone as the reference/tested option.
The **INMP441** microphone was user-confirmed as compatible without firmware changes when using the
same I2S wiring on the same physical XIAO pins (`SCK` -> D3, `WS` -> D1, `SD` -> D2); see:
https://github.com/Sukecz/esp32-birdnet-mic/discussions/25

Firmware 1.21 also provides a 256-fs MCLK on D7 for experimental PCM1808 ADC testing. Existing
ICS-43434 and INMP441 installations leave D7 unconnected and require no wiring change.

### Structure
- `index.html` - main page with the "Flash" button and instructions.
- `manifest.json` - ESP Web Tools manifest, writes only firmware parts without NVS and includes C3/S3/C5/C6 builds.
- `bootloader-<board>.bin`, `partitions-<board>.bin`, `boot_app0-<board>.bin` - system parts for USB web flashing.
- `firmware-<board>.bin` - merged image (bootloader + partition + app), only for manual full flash.
- `firmware-app-<board>.bin` - app-only image for OTA update through the device Web UI.
- `bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`, `firmware-app.bin` - compatible C6 aliases.
- `ota-version.txt` - simple text file with the OTA build version.

The manifest intentionally does not use the merged `firmware.bin` at offset `0x0`, because during an
update it would overwrite the NVS area from `0x9000` with `0xff` bytes and erase Wi-Fi and application
settings. ESP Web Tools also treats flashing an unrecognized device as a new installation and erases
the whole flash by default. Because of that, `new_install_prompt_erase` is enabled in the manifest;
when updating, choose to keep existing data.

### OTA update without USB
1. Open **https://esp32mic.msmeteo.cz**.
2. In the **OTA update without USB** section, enter the device IP address, for example `192.168.1.80`.
3. Click **Open OTA update**.
4. On the device page, choose one option:
   - **Automatic update**: the device downloads the latest board-specific app-only firmware from the web.
   - **Upload compiled file**: manually select an app-only `.bin` file.

Automatic download uses a stable plain HTTP URL based on the compiled board profile, for example
`http://esp32mic.msmeteo.cz/firmware-app-c3.bin`. Deployment replaces each stable board alias with
the latest image and verifies it against the versioned artifact. The server must serve these files
without redirects, with an octet-stream content type and valid content length. HTTPS/TLS does not
fit in the tight default XIAO ESP32-C3/C6 partitions. `firmware-app.bin` remains a C6-compatible
alias for older firmware.

Do not upload `firmware-<board>.bin` or `firmware.bin` on the OTA page. Those are USB merged images.
For OTA, use `firmware-app-<board>.bin`; `firmware-app.bin` is the compatible C6 alias.

### How to prepare firmware-<board>.bin
Generate a merged image, for example:
```bash
esptool.py --chip esp32c6 merge_bin \
  -o firmware-c6.bin \
  0x0 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 firmware-app-c6.bin
```
Or use the output from `idf.py build` / `arduino-esp32` if it produces the merged bin directly.

### Web deployment

1. Put `index.html`, `manifest.json`, all `bootloader*.bin`, `partitions*.bin`,
   `boot_app0*.bin`, `firmware*.bin`, `firmware-app*.bin`, and `ota-version.txt` in the
   static HTTPS hosting root.
2. Make sure the MIME types for `.json` and `.bin` are correct (`application/json`, `application/octet-stream`).
3. Open the page in Chrome/Edge (desktop), allow serial port access, and click "Flash".

### GitHub release
The version is taken from `web-flasher/manifest.json`; the release should include the flasher/OTA
artifacts from this folder.

### Notes
- WebSerial works only in desktop Chrome/Edge (not Safari, not Firefox).
- The page must run over HTTPS (or `http://localhost` for local testing).
