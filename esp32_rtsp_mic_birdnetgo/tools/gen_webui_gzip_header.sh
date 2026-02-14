#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HTML_FILE="${ROOT_DIR}/webui/index.html"
OUT_FILE="${ROOT_DIR}/WebUI_gz.h"

if [[ ! -f "${HTML_FILE}" ]]; then
  echo "Missing HTML source: ${HTML_FILE}" >&2
  exit 1
fi

TMP_GZ="$(mktemp)"
trap 'rm -f "${TMP_GZ}"' EXIT

gzip -n -9 -c "${HTML_FILE}" > "${TMP_GZ}"
GZ_LEN="$(wc -c < "${TMP_GZ}")"

{
  echo "#pragma once"
  echo "#include <Arduino.h>"
  echo "#include <pgmspace.h>"
  echo
  echo "static const uint8_t WEBUI_INDEX_GZ[] PROGMEM = {"
  xxd -i "${TMP_GZ}" | sed '1d' | head -n -2
  echo "};"
  echo
  echo "static const size_t WEBUI_INDEX_GZ_LEN = ${GZ_LEN};"
} > "${OUT_FILE}"

echo "Generated ${OUT_FILE} (${GZ_LEN} bytes gzipped)"
