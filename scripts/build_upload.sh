#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

PORT="${1:-}"

if [[ -z "$PORT" ]]; then
  if command -v pio >/dev/null 2>&1; then
    PORT="$(pio device list | awk '/(usbserial|usbmodem|wchusbserial|ttyUSB|ttyACM)/ {print $1; exit}')"
  else
    PORT="$(arduino-cli board list | awk '/(usbserial|usbmodem|wchusbserial|ttyUSB|ttyACM)/ {print $1; exit}')"
  fi
fi

if [[ -z "$PORT" ]]; then
  echo "Could not auto-detect ESP32 serial port. Pass it explicitly:" >&2
  echo "  ./scripts/build_upload.sh /dev/cu.usbserial-0001" >&2
  exit 1
fi

echo "Building and uploading to $PORT"

FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs"

if command -v pio >/dev/null 2>&1; then
  pio run --target upload --upload-port "$PORT"
else
  arduino-cli compile --fqbn "$FQBN" .
  arduino-cli upload --fqbn "$FQBN" --port "$PORT" .
fi
