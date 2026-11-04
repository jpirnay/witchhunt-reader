#!/usr/bin/env bash
# Builds the lab firmware for the PC and runs lab.py against it. No device involved.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-result \
  -I"$here/host" -I"$here/../src" \
  "$here/../src/Uc8253Lab.cpp" "$here/../src/main.cpp" "$here/host/fake_device.cpp" \
  -o "$build/fake_device"

FAKE_DEVICE="$build/fake_device" python3 "$here/test_lab.py" "$@"
