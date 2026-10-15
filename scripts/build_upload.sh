#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

PORT="${1:-}"
ENVIRONMENT="${2:-}"

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

if [[ "$ENVIRONMENT" == "esp32-c6-lcd-147" || "$ENVIRONMENT" == "esp32-c3-devkitm-1" ]]; then
  if ! command -v pio >/dev/null 2>&1; then
    echo "PlatformIO is required for an explicit environment." >&2
    exit 1
  fi
  pio run --environment "$ENVIRONMENT" --target upload --upload-port "$PORT"
  exit 0
fi

if [[ -n "$ENVIRONMENT" ]]; then
  echo "Unsupported PlatformIO environment: $ENVIRONMENT" >&2
  echo "Supported environments: esp32-c3-devkitm-1, esp32-c6-lcd-147" >&2
  exit 1
fi

FQBN="esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc"

# The ESP32-C3 is connected through native USB. The Arduino CLI board options
# enable USB CDC on boot so Serial output is available over USB.
arduino-cli compile --fqbn "$FQBN" .
arduino-cli upload --fqbn "$FQBN" --port "$PORT" .
