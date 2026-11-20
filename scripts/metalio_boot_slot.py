#!/usr/bin/env python3
"""Inspect the Metalio Plus OTA slots or boot a verified app already on the device."""

import argparse
import binascii
import hashlib
import struct
from pathlib import Path

SECTOR = 4096
S31_CHIP = 32
OTA_STATES_BOOTABLE = {0, 1, 2, 0xFFFFFFFF}


def read_layout(raw):
    partitions = {}
    for index in range(0, len(raw), 32):
        row = raw[index : index + 32]
        if row[:16] == b"\xeb\xeb" + b"\xff" * 14:
            if row[16:] != hashlib.md5(raw[:index]).digest():
                raise ValueError("Partition table checksum mismatch")
            break
        magic, kind, subtype, offset, size, _, flags = struct.unpack("<HBBII16sI", row)
        if magic != 0x50AA:
            raise ValueError("Missing partition table checksum")
        key = (kind, subtype)
        if key in ((0, 0x10), (0, 0x11), (1, 0)):
            if key in partitions or flags or offset % SECTOR or size % SECTOR:
                raise ValueError("Unsupported OTA partition")
            partitions[key] = (offset, size)
    else:
        raise ValueError("Missing partition table checksum")
    offsets = [partitions.get(key) for key in ((1, 0), (0, 0x10), (0, 0x11))]
    allowed = [
        [(0xD000, 0x2000), (0x80000, 0x500000), (0x580000, 0x500000)],
        [(0xD000, 0x2000), (0x10000, 0x500000), (0x510000, 0x500000)],
    ]
    if offsets not in allowed:
        raise ValueError("Unknown Metalio Plus OTA layout")
    return offsets


def app_description(app):
    if len(app) < 112 or app[0] != 0xE9 or struct.unpack_from("<H", app, 12)[0] != S31_CHIP:
        return None
    if app[32:36] != b"\x32\x54\xcd\xab":
        return None
    version = app[48:80].split(b"\0", 1)[0].decode("ascii", "replace")
    project = app[80:112].split(b"\0", 1)[0].decode("ascii", "replace")
    return f"{project} {version}"


def ota_crc(sequence):
    return binascii.crc32(struct.pack("<I", sequence), 0xFFFFFFFF)


def active_selector(metadata):
    candidates = []
    for index in range(2):
        sequence, state, crc = struct.unpack_from("<I20xII", metadata, index * SECTOR)
        if 0 < sequence < 0xFFFFFFFF and state in OTA_STATES_BOOTABLE and ota_crc(sequence) == crc:
            candidates.append((sequence, index, state))
    return max(candidates, default=(0, 1, 0xFFFFFFFF))


def inspect_or_switch(args):
    from esptool.bin_image import LoadFirmwareImage
    from esptool.cmds import detect_chip, write_flash

    rom = detect_chip(args.port, connect_attempts=3)
    connection = rom
    try:
        if rom.CHIP_NAME != "ESP32-S31" or rom.get_security_info()["flags"] != 0:
            raise ValueError("Expected an ESP32-S31 with standard USB boot security settings")
        rom.flash_spi_attach(0)
        connection = rom.run_stub()
        (meta_offset, meta_size), *slots = read_layout(connection.read_flash(0x8000, SECTOR))
        sequence, index, state = active_selector(connection.read_flash(meta_offset, meta_size))
        active = (sequence - 1) % 2 if sequence else 0
        for i, (offset, _) in enumerate(slots):
            description = app_description(connection.read_flash(offset, SECTOR))
            print(f"ota_{i} {offset:#x}: {description or 'no S31 app'}" + (" (selected)" if i == active else ""))
        if args.slot is None or args.slot == active:
            return
        if state in (0, 1):
            raise ValueError("The current application has an unfinished OTA verification")
        app = args.image.read_bytes()
        offset, limit = slots[args.slot]
        if not app_description(app) or len(app) > limit:
            raise ValueError("Expected an S31 application binary that fits the selected slot")
        image = LoadFirmwareImage("esp32s31", app)
        if image.checksum != image.calculate_checksum() or (
            image.append_digest and image.stored_digest != image.calc_digest
        ):
            raise ValueError("Application image checksum mismatch")
        connection.change_baud(921600)
        if connection.read_flash(offset, len(app)) != app:
            raise ValueError("The selected slot must match the supplied S31 application image byte for byte")
        next_sequence = sequence + 1
        if (next_sequence - 1) % 2 != args.slot:
            next_sequence += 1
        if next_sequence >= 0xFFFFFFFF:
            raise ValueError("OTA sequence is exhausted")
        entry = struct.pack("<I20sII", next_sequence, b"\xff" * 20, 2, ota_crc(next_sequence))
        sector = entry.ljust(SECTOR, b"\xff")
        address = meta_offset + (1 - index) * SECTOR
        write_flash(connection, [(address, sector)])
        if connection.read_flash(address, SECTOR) != sector:
            raise ValueError("OTA selector verification failed")
        print(f"Rebooting into ota_{args.slot}")
    finally:
        connection.hard_reset()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--slot", type=int, choices=(0, 1), help="Select an installed, verified app")
    parser.add_argument("--image", type=Path, help="Exact application binary installed in the target slot")
    args = parser.parse_args()
    if (args.slot is None) != (args.image is None):
        parser.error("--slot and --image must be supplied together")
    try:
        inspect_or_switch(args)
    except ValueError as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
