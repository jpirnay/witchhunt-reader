#!/usr/bin/env python3
"""Record the UI language slot layout a past release wrote, for the slot conversion (docs/i18n.md).

Before 2.41 a language's strings reached the flash slot from a pack in the image, one entry per StrId in
the order that firmware's generator gave them: the slot names no key, only an index. To read such a slot
after an update, the firmware needs that release's key order. This script recovers it from the release's
own sources -- it runs the tag's gen_i18n.py over the tag's tree with --strip-unused, as that release's
build did -- and writes lib/I18n/legacy/<tag>.keys, which gen_i18n.py turns into the conversion tables.

Usage:
    python3 scripts/i18n_legacy_layout.py 2.40 [2.41RC ...]

The output is committed; run this only for a newly named release.
"""

import io
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT_DIR = REPO / "lib" / "I18n" / "legacy"


def enum_members(header: str, name: str) -> list[str]:
    match = re.search(r"enum class " + name + r" : \w+ \{(.*?)\};", header, re.S)
    if not match:
        raise ValueError(f"enum {name} not found")
    members = []
    for line in match.group(1).splitlines():
        line = line.split("//")[0].strip().rstrip(",")
        if not line or line == "_COUNT":
            continue
        members.append(line.split("=")[0].strip())
    return members


def slot_format(tree: Path) -> int:
    text = (tree / "lib" / "hal" / "FlashLangPartition.h").read_text(encoding="utf-8")
    match = re.search(r"FORMAT_VERSION\s*=\s*(\d+)", text)
    if not match:
        raise ValueError("FlashLangPartition.h states no FORMAT_VERSION")
    return int(match.group(1))


def layout(tag: str) -> str:
    archive = subprocess.run(
        ["git", "-C", str(REPO), "archive", tag, "src", "lib", "scripts"], capture_output=True, check=True
    ).stdout
    with tempfile.TemporaryDirectory() as tmp:
        tree = Path(tmp)
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            tar.extractall(tree, filter="data")
        out = tree / "out"
        out.mkdir()
        subprocess.run(
            [sys.executable, "scripts/gen_i18n.py", "lib/I18n/translations", "out", "--strip-unused", "--force"],
            cwd=tree, check=True, capture_output=True,
        )
        keys_header = (out / "I18nKeys.h").read_text(encoding="utf-8")
        keys = enum_members(keys_header, "StrId")
        languages = enum_members(keys_header, "Language")
        fmt = slot_format(tree)
    lines = [
        f"# The UI language slot release {tag} wrote: written by scripts/i18n_legacy_layout.py from the tag.",
        "# Do not edit. gen_i18n.py reads it to convert such a slot (docs/i18n.md, Updating from 2.40).",
        f"release {tag}",
        f"slot_format {fmt}",
        f"languages {' '.join(languages)}",
        f"keys {len(keys)}",
    ]
    lines += keys
    return "\n".join(lines) + "\n"


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for tag in sys.argv[1:]:
        text = layout(tag)
        path = OUT_DIR / f"{tag}.keys"
        path.write_text(text, encoding="utf-8")
        print(f"Wrote {path.relative_to(REPO)}")


if __name__ == "__main__":
    main()
