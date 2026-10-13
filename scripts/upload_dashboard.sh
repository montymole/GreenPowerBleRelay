#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 PORT [esp32-c3-devkitm-1|esp32-c6-lcd-147]" >&2
  exit 1
fi
environment="${2:-esp32-c3-devkitm-1}"
case "$environment" in
  esp32-c3-devkitm-1|esp32-c6-lcd-147) ;;
  *) echo "Unsupported environment: $environment" >&2; exit 1 ;;
esac

# PlatformIO's pre-build hook generates the gzip assets before making the image.
pio run --environment "$environment" --target uploadfs --upload-port "$1"
