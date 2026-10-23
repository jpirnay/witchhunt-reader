#!/usr/bin/env python3
from __future__ import annotations
"""Generate the font manifests from a directory of .cpfont, .ttf and .otf files.

Scans the input directory (flat or nested by family) for font files, reads
their headers for the styles they hold, and writes the JSON manifests the
reader downloads fonts from:

- --output (fonts.json): version 2, the .cpfont families only. Firmware
  that reads versions 1 and 2 only lists every family it finds and would
  download an outline one it cannot install.
- --output-v3 (fonts-v3.json): version 3, every family, each with its
  "format" ("cpfont", "ttf" or "otf") and, where the config says so,
  "inDownloadAll": false.

Usage:
    python3 scripts/generate-font-manifest.py \
        --input lib/EpdFont/scripts/output \
        --base-url "https://example.com/fonts/" \
        --output dist/fonts.json --output-v3 dist/fonts-v3.json \
        --config lib/EpdFont/scripts/sd-fonts.yaml

The input directory may be flat (all files in one dir) or nested (family
subdirectories). Family names are derived from filenames using the
conventions <FamilyName>_<size>.cpfont and <FamilyName>-<Style>.ttf/.otf.
With --config, only the families sd-fonts.yaml lists are written, with its
descriptions; a folder it does not list is reported and left out.
"""

import argparse
import json
import os
import struct
import sys
import zlib
from pathlib import Path

# --- .cpfont binary format constants ---
# Global header: 8s magic, H version, H flags, B styleCount, 19x reserved
GLOBAL_HEADER_FORMAT = "<8sHHB19x"
GLOBAL_HEADER_SIZE = struct.calcsize(GLOBAL_HEADER_FORMAT)  # 32 bytes

# Style TOC entry: B styleId, 3x pad, I intervalCount, I glyphCount, ...
# We only need the first byte (styleId) from each 32-byte entry.
STYLE_TOC_ENTRY_SIZE = 32
STYLE_TOC_ENTRY_FORMAT = "<B31x"

CPFONT_MAGIC = b"CPFONT\x00\x00"
CPFONT_VERSION = 4

STYLE_NAMES = {0: "regular", 1: "bold", 2: "italic", 3: "bolditalic"}

OUTLINE_SUFFIXES = {".ttf": "ttf", ".otf": "otf"}


def load_config(yaml_path: Path) -> dict[str, dict]:
    """The families sd-fonts.yaml lists, by name."""
    import yaml

    with open(yaml_path, encoding="utf-8") as f:
        config = yaml.safe_load(f)
    return {f["name"]: f for f in config.get("families", [])}


def read_cpfont_styles(filepath: Path) -> list[str]:
    """Read style names from a .cpfont file's binary header."""
    with open(filepath, "rb") as f:
        header_data = f.read(GLOBAL_HEADER_SIZE)
        if len(header_data) < GLOBAL_HEADER_SIZE:
            print(f"  WARNING: {filepath.name} too small, skipping", file=sys.stderr)
            return []

        magic, version, _flags, style_count = struct.unpack(
            GLOBAL_HEADER_FORMAT, header_data
        )

        if magic != CPFONT_MAGIC:
            print(
                f"  WARNING: {filepath.name} bad magic {magic!r}, skipping",
                file=sys.stderr,
            )
            return []

        if version != CPFONT_VERSION:
            print(
                f"  WARNING: {filepath.name} version {version} != {CPFONT_VERSION}, skipping",
                file=sys.stderr,
            )
            return []

        styles = []
        for _ in range(style_count):
            toc_data = f.read(STYLE_TOC_ENTRY_SIZE)
            if len(toc_data) < STYLE_TOC_ENTRY_SIZE:
                break
            (style_id,) = struct.unpack(STYLE_TOC_ENTRY_FORMAT, toc_data)
            name = STYLE_NAMES.get(style_id, f"unknown({style_id})")
            styles.append(name)

        return styles


def read_outline_style(filepath: Path) -> str:
    """An outline (TrueType or CFF) file's style from head.macStyle (bit 0 bold, bit 1 italic), as the reader's
    discovery reads it."""
    with open(filepath, "rb") as f:
        data = f.read(12)
        num_tables = struct.unpack(">H", data[4:6])[0]
        records = f.read(16 * num_tables)
        for i in range(num_tables):
            tag, _checksum, offset, _length = struct.unpack(">4sIII", records[16 * i:16 * i + 16])
            if tag == b"head":
                f.seek(offset + 44)
                mac_style = struct.unpack(">H", f.read(2))[0]
                return STYLE_NAMES[mac_style & 3]
    raise ValueError(f"{filepath.name}: no head table")


def compute_crc32(filepath: Path) -> int:
    # Matches the on-device esp_rom_crc32_le(0, buf, len) accumulator used by
    # FontDownloadActivity::computeFileCrc32. Originally introduced upstream in
    # crosspoint-reader PR #1904 ("verify CRC32 checksum for font files").
    crc = 0
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def parse_filename(filename: str) -> tuple[str, str] | None:
    """Parse '<FamilyName>_<size>.cpfont' into (family, size_str).

    Returns None if the filename doesn't match the expected pattern.
    """
    if not filename.endswith(".cpfont"):
        return None
    stem = filename[: -len(".cpfont")]
    parts = stem.rsplit("_", 1)
    if len(parts) != 2:
        return None
    family, size_str = parts
    if not size_str.isdigit():
        return None
    return family, size_str


def scan_font_files(input_dir: Path) -> dict[str, list[Path]]:
    """Scan input directory for .cpfont, .ttf and .otf files, grouped by family name.

    Handles both flat and nested directory layouts. A family is one format: a folder mixing them is an error.
    """
    families: dict[str, list[Path]] = {}

    for path in sorted(input_dir.rglob("*.cpfont")):
        if not path.is_file():
            continue
        parsed = parse_filename(path.name)
        if parsed is None:
            print(f"  WARNING: skipping {path.name} (unexpected filename format)", file=sys.stderr)
            continue
        family_name = parsed[0]
        families.setdefault(family_name, []).append(path)

    for suffix in OUTLINE_SUFFIXES:
        for path in sorted(input_dir.rglob(f"*{suffix}")):
            if not path.is_file():
                continue
            # <Family>-<Style>.otf: the family is the name before the last "-".
            families.setdefault(path.stem.rsplit("-", 1)[0], []).append(path)

    for family_name, files in families.items():
        if len({p.suffix for p in files}) > 1:
            suffixes = ", ".join(sorted({p.suffix for p in files}))
            raise ValueError(f"family {family_name} has files of two formats ({suffixes})")

    return families


def family_format(files: list[Path]) -> str:
    """"cpfont", "ttf" or "otf": the format of a family's files."""
    return OUTLINE_SUFFIXES.get(files[0].suffix, "cpfont") if files else "cpfont"


def build_manifest(
    families: dict[str, list[Path]], base_url: str, input_dir: Path, config: dict[str, dict] | None = None,
    version: int = 2,
) -> dict:
    """Build the manifest dict from discovered font families: version 2 holds the .cpfont families, version 3
    every family with its format."""
    manifest_families = []

    for family_name in sorted(families.keys()):
        files = families[family_name]
        fmt = family_format(files)
        if version < 3 and fmt != "cpfont":
            continue

        if fmt == "cpfont":
            # Read styles from the first file (all files in a family have the
            # same styles since they're generated from the same source fonts).
            styles = read_cpfont_styles(files[0]) if files else []
        else:
            styles = [read_outline_style(f) for f in sorted(files, key=lambda p: p.name)]

        # Get description
        entry = (config or {}).get(family_name, {})
        description = entry.get("description")
        if description is None:
            print(f"  WARNING: no description for family '{family_name}'", file=sys.stderr)
            description = family_name

        file_entries = []
        for filepath in sorted(files, key=lambda p: p.name):
            file_entries.append(
                {
                    "name": filepath.relative_to(input_dir).as_posix(),
                    "size": filepath.stat().st_size,
                    "crc32": compute_crc32(filepath),
                }
            )

        family_entry = {"name": family_name, "description": description}
        if version >= 3:
            family_entry["format"] = fmt
            if not entry.get("inDownloadAll", True):
                family_entry["inDownloadAll"] = False
        family_entry["styles"] = styles
        family_entry["files"] = file_entries
        manifest_families.append(family_entry)

    return {
        # v2 adds per-file crc32 (kept optional on the device side so old v1
        # manifests still load — see FontDownloadActivity::fetchAndParseManifest);
        # v3 each family's format, and whether "Download all" takes it.
        "version": version,
        "baseUrl": base_url,
        "families": manifest_families,
    }


def write_manifest(manifest: dict, output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8", newline="\n") as f:
        # ensure_ascii=False: the firmware's streaming parser does not decode unicode escapes (backslash-u).
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"Wrote {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Generate the font manifests from .cpfont, .ttf and .otf files"
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Directory containing the font files (flat or nested by family)",
    )
    parser.add_argument(
        "--base-url",
        required=True,
        help="URL prefix for font downloads (device concatenates baseUrl + filename)",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output path for fonts.json (version 2: .cpfont families)",
    )
    parser.add_argument(
        "--output-v3",
        default=None,
        help="Output path for fonts-v3.json (version 3: every family, with its format)",
    )
    parser.add_argument(
        "--config",
        default=None,
        help="sd-fonts.yaml: only its families are written, with its descriptions (default: every family found)",
    )
    args = parser.parse_args()

    input_dir = Path(args.input)
    if not input_dir.is_dir():
        print(f"ERROR: {input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    # Ensure base URL ends with /
    base_url = args.base_url
    if not base_url.endswith("/"):
        base_url += "/"

    config = None
    if args.config:
        config = load_config(Path(args.config))
        print(f"Loaded {len(config)} families from {args.config}")

    print(f"Scanning {input_dir} for .cpfont, .ttf and .otf files...")
    try:
        families = scan_font_files(input_dir)
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    if config is not None:
        # Only what the config lists is hosted: a folder left behind (a family taken out of it) is not offered.
        for name in sorted(set(families) - set(config)):
            print(f"  WARNING: {name} is not in {args.config}; left out of the manifests", file=sys.stderr)
            del families[name]
        for name, files in families.items():
            wanted = config[name].get("format", "cpfont")
            if family_format(files) != wanted:
                print(f"ERROR: {name} is {wanted} in {args.config} but its files are not", file=sys.stderr)
                sys.exit(1)

    if not families:
        print("ERROR: no font files found", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(families)} font families:")
    for name, files in sorted(families.items()):
        print(f"  {name}: {len(files)} files")

    write_manifest(build_manifest(families, base_url, input_dir, config, 2), Path(args.output))
    if args.output_v3:
        write_manifest(build_manifest(families, base_url, input_dir, config, 3), Path(args.output_v3))


if __name__ == "__main__":
    main()
