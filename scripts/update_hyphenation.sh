#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

mkdir -p build

process_liang() {
  local lang="$1"

  wget -O "build/$lang.bin" \
    "https://github.com/typst/hypher/raw/refs/heads/main/tries/$lang.bin"

  python3 scripts/generate_hyphenation_trie.py \
    --input "build/$lang.bin" \
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

# German deliberately no longer uses the ~201 KiB Hypher trie.  Keep the old
# generated header in the repository while the new implementation is being
# evaluated, but do not refresh/reference it in normal builds.
if [[ "${GENERATE_LEGACY_GERMAN_TRIE:-0}" == "1" ]]; then
  process_liang de
fi

# DANTE's annotated word list is the source for the compact German validator.
# The generator embeds the source SHA-256 in the generated header so a build can
# always be traced to the exact downloaded word list even though master evolves.
wget -O build/dante-german-wortliste \
  "https://github.com/hyphenation/languages-german/raw/refs/heads/master/wortliste"

python3 scripts/generate_german_hybrid_rules.py \
  --input build/dante-german-wortliste \
  --output-header lib/Epub/Epub/hyphenation/generated/de_hybrid_rules.h \
  --output-eval test/hyphenation_eval/resources/german_dante_eval.txt
