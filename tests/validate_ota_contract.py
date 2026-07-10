#!/usr/bin/env python3
import json
import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
SKETCH = ROOT / "esp32-birdnet-mic" / "esp32-birdnet-mic.ino"
WEBUI = ROOT / "esp32-birdnet-mic" / "WebUI.cpp"
WEB = ROOT / "web-flasher"
MANUAL = ROOT / "manual-ota-firmware"


def require(condition, message):
    if not condition:
        raise SystemExit(message)


sketch = SKETCH.read_text(encoding="utf-8")
webui = WEBUI.read_text(encoding="utf-8")
version_match = re.search(r'^\s*#define\s+FW_VERSION\s+"([^"]+)"', sketch, re.M)
require(version_match, "FW_VERSION not found")
version = version_match.group(1)
require(re.fullmatch(r"[0-9]+\.[0-9]+", version), "FW_VERSION must use major.minor format")

profiles = {
    "ARDUINO_XIAO_ESP32C3": "firmware-app-c3.bin",
    "ARDUINO_XIAO_ESP32S3": "firmware-app-s3.bin",
    "ARDUINO_XIAO_ESP32C5": "firmware-app-c5.bin",
    "ARDUINO_XIAO_ESP32C6": "firmware-app-c6.bin",
}
for macro, artifact in profiles.items():
    pattern = rf"defined\({macro}\).*?#define\s+XIAO_OTA_ARTIFACT\s+\"{re.escape(artifact)}\""
    require(re.search(pattern, sketch, re.S), f"Missing OTA artifact mapping for {macro}")
require('#define XIAO_OTA_ARTIFACT ""' in sketch, "Generic builds must disable automatic OTA")

default_url = re.search(r"static String getDefaultOtaUrl\(\)\s*\{(.*?)\n\}", webui, re.S)
require(default_url, "getDefaultOtaUrl not found")
require("FW_OTA_ARTIFACT_STR" in default_url.group(1), "Default OTA URL must use board artifact")
require("FW_VERSION_STR" not in default_url.group(1), "Default OTA URL must not use current firmware version")
require("name='url'" not in webui, "Automatic OTA URL must not be editable")
require("String url = getDefaultOtaUrl();" in webui, "OTA install endpoint must use the compiled board URL")
require("return expected.length() > 0 && url == expected;" in webui, "OTA URL validation must require an exact board URL")

manifest = json.loads((WEB / "manifest.json").read_text(encoding="utf-8"))
require(manifest["version"] == version, "Manifest version does not match FW_VERSION")
require((WEB / "ota-version.txt").read_text(encoding="utf-8").strip() == version,
        "ota-version.txt does not match FW_VERSION")

for key in ("c3", "s3", "c5", "c6"):
    stable = WEB / f"firmware-app-{key}.bin"
    versioned = WEB / f"firmware-app-{key}-{version}.bin"
    manual = MANUAL / f"firmware-app-{key}.bin"
    for path in (stable, versioned, manual):
        require(path.is_file() and path.stat().st_size > 0, f"Missing OTA artifact: {path}")
    require(stable.read_bytes() == versioned.read_bytes(), f"Stable/versioned mismatch for {key}")
    require(stable.read_bytes() == manual.read_bytes(), f"Web/manual mismatch for {key}")

require((WEB / "firmware-app-c6.bin").read_bytes() == (WEB / "firmware-app.bin").read_bytes(),
        "C6 compatibility alias mismatch")
print(f"OTA release contract valid for firmware {version}")
