#!/bin/bash
# fetch_idf.sh <dir>: the ESP-IDF sources gen_sections_ld.py needs, at the commit pioarduino's prebuilt C3 libraries were
# built from (framework-arduinoespressif32-libs/versions.txt: "esp-idf: v5.5.5 b774170ff46"): the linker fragments,
# esp_system's linker-script templates and ldgen. A shallow, blobless, sparse checkout: a few hundred kilobytes.
set -euo pipefail
DIR=${1:?usage: fetch_idf.sh <dir>}
COMMIT=b774170ff46c393eeb5e495ea37936038d3f4f4f
mkdir -p "$DIR" && cd "$DIR"
git init -q
git remote add origin https://github.com/espressif/esp-idf 2>/dev/null || true
git sparse-checkout set --no-cone '/components/**/*.lf' '/components/esp_system/ld/' '/tools/ldgen/'
git fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
git checkout -q FETCH_HEAD
test "$(git rev-parse HEAD)" = "$COMMIT"
