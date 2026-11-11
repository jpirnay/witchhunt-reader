#!/usr/bin/env python3
"""
Build and check a release's full-install image: bootloader, partition table, otadata and app in one
file, written from flash offset 0x0.

A release's firmware*.bin is the app alone, and an OTA, SD-card or app-only update leaves the
bootloader and the partition table the device already has. That is not enough for a device whose
table is not ours (a stock X4 Pro has no spiffs partition), nor for an X3/X4 that should get the
C3 bootloader of bootloader/esp32c3 (scripts/c3_bootloader.py). The full image carries both.

It is merged from exactly what `pio run -t upload` writes for the env: the images, offsets, flash
mode and frequency recorded by scripts/flash_layout.py in the build folder, and the same esptool
(merge-bin, as the platform's own firmware.factory.bin). The app at the app offset is the given
firmware.bin byte for byte, so a release passes the one it publishes, after
scripts/patch_min_chip_rev.py. merge-bin fills the gaps with 0xFF, so the nvs partition is cleared
and stops at the app's end: the rest of the flash is not part of the image.

Then every image is checked against its sources, independently of esptool:
  - the bootloader header names the env's flash mode, frequency and size, and the bootloader is the
    source file with just that header change (and its SHA-256 digest recomputed), which is what
    the upload's write-flash makes of it; on the C3 the source is bootloader/esp32c3/bootloader.bin;
  - the table at 0x8000 is partitions.bin, its MD5 entry holds, and it parses to the partitions CSV;
  - otadata holds boot_app0.bin, so the device boots app0;
  - the app offset holds the app byte for byte;
  - every byte between the images is 0xFF.

Usage:
  full_install_image.py <build dir> --out <image> [--app <firmware.bin>]
  full_install_image.py <build dir> --check <image> [--app <firmware.bin>]
Exit status 0 when every check passes.
"""

import argparse
import csv
import hashlib
import io
import json
import os
import struct
import subprocess
import sys

# esptool's header codes for the ESP32-C3 and ESP32-S3 (esptool/targets/esp32.py and children).
FLASH_MODES = {"qio": 0, "qout": 1, "dio": 2, "dout": 3}
FLASH_SIZES = {"1MB": 0x0, "2MB": 0x1, "4MB": 0x2, "8MB": 0x3, "16MB": 0x4, "32MB": 0x5, "64MB": 0x6, "128MB": 0x7}
FLASH_FREQS = {"80m": 0xF, "40m": 0x0, "26m": 0x1, "20m": 0x2}
SUPPORTED_CHIPS = ("esp32c3", "esp32s3")

# ESP-IDF partition table: 32-byte entries, then an MD5 entry, then 0xFF.
PART_MAGIC = b"\xaa\x50"
PART_MD5_MAGIC = b"\xeb\xeb" + b"\xff" * 14
PART_TYPES = {"app": 0x00, "data": 0x01}
PART_SUBTYPES = {
    "app": {"factory": 0x00, "test": 0x20, **{"ota_%d" % i: 0x10 + i for i in range(16)}},
    "data": {"ota": 0x00, "phy": 0x01, "nvs": 0x02, "coredump": 0x03, "nvs_keys": 0x04, "efuse": 0x05,
             "undefined": 0x06, "esphttpd": 0x80, "fat": 0x81, "spiffs": 0x82, "littlefs": 0x83},
}
PART_FLAGS = {"encrypted": 0x1, "readonly": 0x2}

C3_BOOTLOADER = os.path.join("bootloader", "esp32c3", "bootloader.bin")


class CheckFailed(Exception):
    pass


def check(condition, message):
    if not condition:
        raise CheckFailed(message)


def hex_int(text):
    return int(str(text), 0)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def load_layout(build_dir):
    path = os.path.join(build_dir, "flash_layout.json")
    if not os.path.isfile(path):
        raise CheckFailed("%s is missing: build the env first (scripts/flash_layout.py writes it)" % path)
    with open(path, encoding="utf-8") as f:
        layout = json.load(f)
    for key in ("chip", "flash_mode", "flash_freq", "flash_size", "partitions_csv", "esptool"):
        check(layout.get(key), "flash_layout.json has no %s" % key)
    check(layout["chip"] in SUPPORTED_CHIPS, "chip %s is not one this script knows" % layout["chip"])
    check(layout["app"].get("offset"), "flash_layout.json has no app offset")
    check(len(layout.get("images", [])) == 3, "expected bootloader, partitions and otadata images")
    return layout


def merge(layout, app, out):
    cmd = [layout["esptool"], "--chip", layout["chip"], "merge-bin", "-o", out,
           "--flash-mode", layout["flash_mode"], "--flash-freq", layout["flash_freq"],
           "--flash-size", layout["flash_size"]]
    for image in layout["images"]:
        cmd += [image["offset"], image["path"]]
    cmd += [layout["app"]["offset"], app]
    print("$ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def expected_bootloader(source, layout):
    """The bootloader as write-flash leaves it: header bytes 2-3 set, appended SHA-256 redone."""
    data = bytearray(source)
    check(data[0] == 0xE9, "bootloader source has no image magic")
    data[2] = FLASH_MODES[layout["flash_mode"]]
    data[3] = (FLASH_SIZES[layout["flash_size"]] << 4) | FLASH_FREQS[layout["flash_freq"]]
    if data[23] == 1:  # hash_appended: the last 32 bytes are the SHA-256 of all before them
        body = bytes(data[:-32])
        data[-32:] = hashlib.sha256(body).digest()
    return bytes(data)


def parse_table(blob):
    entries = []
    md5 = hashlib.md5()
    for pos in range(0, len(blob), 32):
        entry = blob[pos:pos + 32]
        if entry[:2] == PART_MAGIC:
            ptype, subtype, offset, size = struct.unpack_from("<BBII", entry, 2)
            label = entry[12:28].split(b"\0", 1)[0].decode("ascii")
            flags = struct.unpack_from("<I", entry, 28)[0]
            entries.append((label, ptype, subtype, offset, size, flags))
            md5.update(entry)
            continue
        if entry[:16] == PART_MD5_MAGIC:
            check(entry[16:] == md5.digest(), "partition table MD5 does not match its entries")
            return entries
        check(entry == b"\xff" * 32, "partition table entry at +0x%x is neither an entry nor the end" % pos)
        raise CheckFailed("partition table has no MD5 entry")
    raise CheckFailed("partition table runs off its sector")


def parse_csv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        lines = [line for line in f if line.strip() and not line.lstrip().startswith("#")]
    for row in csv.reader(io.StringIO("".join(lines))):
        row = [cell.strip() for cell in row]
        name, ptype, subtype, offset, size = row[:5]
        flags = 0
        for flag in (row[5].split(":") if len(row) > 5 and row[5] else []):
            flags |= PART_FLAGS[flag.strip()]
        type_code = PART_TYPES.get(ptype, None)
        type_code = type_code if type_code is not None else hex_int(ptype)
        sub_map = PART_SUBTYPES.get(ptype, {})
        sub_code = sub_map[subtype] if subtype in sub_map else hex_int(subtype)
        rows.append((name, type_code, sub_code, hex_int(offset), hex_int(size), flags))
    return rows


def verify(image, layout, app_path, project_dir):
    chip = layout["chip"]
    results = []
    spans = []  # (start, end, name) of every image placed in the file

    def ok(text):
        results.append(text)

    bl_entry, pt_entry, ota_entry = layout["images"]
    check(hex_int(bl_entry["offset"]) == 0, "bootloader is not at 0x0")

    # Bootloader: header and bytes.
    check(image[0] == 0xE9, "no image magic at 0x0")
    want_mode = FLASH_MODES[layout["flash_mode"]]
    want_sizefreq = (FLASH_SIZES[layout["flash_size"]] << 4) | FLASH_FREQS[layout["flash_freq"]]
    check(image[2] == want_mode, "bootloader flash mode byte %d, env wants %s" % (image[2], layout["flash_mode"]))
    check(image[3] == want_sizefreq, "bootloader size/freq byte 0x%02x, env wants 0x%02x" % (image[3], want_sizefreq))
    ok("bootloader header: %s, %s, %s (0x%02x 0x%02x)" % (layout["flash_mode"], layout["flash_freq"],
                                                       layout["flash_size"], image[2], image[3]))
    if chip == "esp32c3":
        ours = os.path.realpath(os.path.join(project_dir, C3_BOOTLOADER))
        check(os.path.realpath(bl_entry["path"]) == ours,
              "C3 bootloader is %s, not %s (scripts/c3_bootloader.py)" % (bl_entry["path"], C3_BOOTLOADER))
    bootloader = expected_bootloader(read(bl_entry["path"]), layout)
    check(image[:len(bootloader)] == bootloader, "bootloader bytes differ from %s as write-flash writes it"
          % bl_entry["path"])
    spans.append((0, len(bootloader), "bootloader"))
    ok("bootloader: %s, %d B, as the upload writes it" % (os.path.relpath(bl_entry["path"], project_dir)
                                                        if bl_entry["path"].startswith(project_dir)
                                                        else os.path.basename(bl_entry["path"]),
                                                        len(bootloader)))

    # Partition table.
    pt_offset = hex_int(pt_entry["offset"])
    table_bin = read(pt_entry["path"])
    check(image[pt_offset:pt_offset + len(table_bin)] == table_bin, "0x%x does not hold partitions.bin" % pt_offset)
    spans.append((pt_offset, pt_offset + len(table_bin), "partitions"))
    entries = parse_table(image[pt_offset:pt_offset + 0x1000])
    csv_rows = parse_csv(layout["partitions_csv"])
    check(entries == csv_rows, "partition table %s does not match %s" % (entries, csv_rows))
    ok("partition table at 0x%x: %d entries = %s" % (pt_offset, len(entries), os.path.basename(layout["partitions_csv"])))

    # otadata.
    ota_offset = hex_int(ota_entry["offset"])
    boot_app0 = read(ota_entry["path"])
    check(image[ota_offset:ota_offset + len(boot_app0)] == boot_app0, "0x%x does not hold boot_app0.bin" % ota_offset)
    otadata = [e for e in entries if e[1] == 0x01 and e[2] == 0x00]
    check(otadata and otadata[0][3] == ota_offset and otadata[0][4] == len(boot_app0),
          "boot_app0.bin does not fill the otadata partition")
    spans.append((ota_offset, ota_offset + len(boot_app0), "otadata"))
    ok("otadata at 0x%x = boot_app0.bin (%d B)" % (ota_offset, len(boot_app0)))

    # App.
    app_offset = hex_int(layout["app"]["offset"])
    app = read(app_path)
    check(image[app_offset:app_offset + len(app)] == app, "0x%x does not hold %s byte for byte" % (app_offset, app_path))
    check(len(image) == app_offset + len(app), "image runs %d B past the app" % (len(image) - app_offset - len(app)))
    ota0 = [e for e in entries if e[1] == 0x00 and e[2] == 0x10]
    check(ota0 and ota0[0][3] == app_offset and len(app) <= ota0[0][4], "app does not fit app0 at 0x%x" % app_offset)
    spans.append((app_offset, len(image), "app"))
    min_rev = struct.unpack_from("<H", app, 15)[0]
    ok("app at 0x%x = %s (%d B, min_chip_rev_full v%d.%d)" % (app_offset, os.path.basename(app_path), len(app),
                                                             min_rev // 100, min_rev % 100))

    # Gaps.
    spans.sort()
    for (_, end, name), (start, _, nxt) in zip(spans, spans[1:]):
        check(end <= start, "%s overlaps %s" % (name, nxt))
        check(image[end:start] == b"\xff" * (start - end), "gap 0x%x-0x%x (%s..%s) is not all 0xFF" % (end, start, name, nxt))
    nvs = [e for e in entries if e[1] == 0x01 and e[2] == 0x02]
    if nvs:
        lo, hi = nvs[0][3], nvs[0][3] + nvs[0][4]
        check(image[lo:hi] == b"\xff" * (hi - lo), "nvs 0x%x-0x%x is not erased" % (lo, hi))
        ok("gaps 0xFF, nvs 0x%x-0x%x erased" % (lo, hi))
    flash_bytes = 1 << (20 + FLASH_SIZES[layout["flash_size"]])
    check(len(image) <= flash_bytes, "image is larger than the flash")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("build_dir", help="the env's build folder, e.g. .pio/build/gh_release")
    parser.add_argument("--app", help="the app image (default: the build's firmware.bin)")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--out", help="write the full image here, then check it")
    group.add_argument("--check", help="only check this full image")
    args = parser.parse_args()

    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        layout = load_layout(args.build_dir)
        app = args.app or layout["app"]["path"]
        target = args.out or args.check
        if args.out:
            merge(layout, app, args.out)
        results = verify(read(target), layout, app, project_dir)
    except (CheckFailed, subprocess.CalledProcessError, OSError, ValueError, KeyError) as e:
        print("full_install_image: FAILED: %s" % e, file=sys.stderr)
        return 1
    print("%s (%s, %d B):" % (target, layout["env"], os.path.getsize(target)))
    for line in results:
        print("  ok  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
