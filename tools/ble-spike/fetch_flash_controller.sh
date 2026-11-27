#!/bin/bash
# SPIKE ONLY: fetch Espressif's flash-only C3 BLE controller for scripts/ble_spike_flash_ctrl.py, from
# espressif/esp32c3-bt-lib at the commit ESP-IDF b774170ff46 (the prebuilt Arduino 3.3.11 libraries) pins, and store it
# under the ROM-assisted library's name so sections.ld's *libbtdm_app.a rules still apply to it.
set -euo pipefail
COMMIT=0a08c4b32f3666003080b662a1a61794da24ff0f
BLOB_GIT_SHA=05cc82f93c20524bf239ac198d2e6ffff6cf1ce7
DIR="$(cd "$(dirname "$0")/../.." && pwd)/.pio/ble-spike/flash-controller"
mkdir -p "$DIR"
curl -fsSL -o "$DIR/libbtdm_app.a" \
  "https://raw.githubusercontent.com/espressif/esp32c3-bt-lib/$COMMIT/esp32c3/libbtdm_app_flash.a"
test "$(git hash-object "$DIR/libbtdm_app.a")" = "$BLOB_GIT_SHA"
echo "ok: $DIR/libbtdm_app.a"
