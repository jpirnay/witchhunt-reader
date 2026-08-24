#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

mkdir -p build

# Set FORCE_HYPHENATION_DOWNLOAD=1 when you explicitly want to refresh the
# downloaded source files. Normal runs reuse files already present in build/.
FORCE_DOWNLOAD="${FORCE_HYPHENATION_DOWNLOAD:-0}"

fetch_if_missing() {
  local url="$1"
  local output="$2"

  if [[ "$FORCE_DOWNLOAD" == "1" || ! -s "$output" ]]; then
    echo "Downloading $(basename "$output")"
    wget -O "$output" "$url"
  else
    echo "Using cached $(basename "$output")"
  fi
}

process_liang() {
  local lang="$1"
  local output="build/$lang.bin"
  local url="https://github.com/typst/hypher/raw/refs/heads/main/tries/$lang.bin"

  fetch_if_missing "$url" "$output"

  python3 scripts/generate_hyphenation_trie.py \
    --input "$output" \
    --output "lib/Epub/Epub/hyphenation/generated/hyph-${lang}.trie.h"
}

# Languages that still use the serialized Liang/Hypher automaton.
process_liang en
process_liang fr
process_liang es
process_liang ru
process_liang it
process_liang uk
process_liang sv
process_liang pl
process_liang pt
process_liang nl

# German deliberately no longer uses the large Hypher trie.
if [[ "${GENERATE_LEGACY_GERMAN_TRIE:-0}" == "1" ]]; then
  process_liang de
fi

DANTE_FILE="build/dante-german-wortliste"
DANTE_URL="https://github.com/hyphenation/languages-german/raw/refs/heads/master/wortliste"
fetch_if_missing "$DANTE_URL" "$DANTE_FILE"

python3 scripts/generate_german_hybrid_rules.py \
  --input "$DANTE_FILE" \
  --output-header lib/Epub/Epub/hyphenation/generated/de_hybrid_rules.h \
  --output-eval test/hyphenation_eval/resources/german_dante_eval.txt
