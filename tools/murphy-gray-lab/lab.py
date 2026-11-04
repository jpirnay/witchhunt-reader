#!/usr/bin/env python3
"""Host side of the Murphy M3 gray lab. See README.md for the tests and the stop rules.

Every step is one run of this script, so a person can photograph the panel between steps. Each step
re-sends everything it needs, because the firmware keeps no state worth trusting across a reset.

--dry-run prints what a step would send without opening a port.
"""
import argparse
import base64
import json
import os
import sys
import time
from pathlib import Path

WIDTH, HEIGHT = 240, 416  # controller-native
ROW_BYTES = WIDTH // 8
PLANE_BYTES = ROW_BYTES * HEIGHT
LUT_BYTES = 42

REST, VSH, VSL = 0, 1, 2  # UC8253c level codes: 0 V, toward black, toward white
LEVEL_NAME = {0: "rest", 1: "VSH", 2: "VSL", 3: "VDHR"}

# LUT registers by what selects them. CDI 0x97 sets DDX=01, whose {NEW,OLD} table gives these
# (DTM1, DTM2) pairs. Data 1 is white, as the datasheet's WW for {1,1} and every production frame say.
VCOM, WW, KW, WK, KK = 0, 1, 2, 3, 4
SLOT_PLANES = {WW: (1, 1), KW: (0, 1), WK: (1, 0), KK: (0, 0)}

# vsl vdhr vcom boost1 pll, as the firmware's CFG takes them
CONFIGS = {
    "prod": bytes([0x3B, 0x0D, 0x0F, 0xD7, 0x09]),  # Murphy production
    "recipe": bytes([0x3B, 0x0D, 0x03, 0xDF, 0x09]),  # the bench recipe
    "oem-alt": bytes([0x3F, 0x03, 0x00, 0xD7, 0x09]),  # OEM alternate init: symmetric rails
}


def lut(*phases):
    """A 42-byte LUT from (level, frames) pairs, four sub-phases to a group."""
    subs = []
    for level, frames in phases:
        while frames > 0:
            step = min(frames, 63)
            subs.append(level << 6 | step)
            frames -= step
    out = bytearray()
    for i in range(0, len(subs), 4):
        chunk = subs[i : i + 4]
        out += bytes([1, *chunk, *[0] * (4 - len(chunk)), 1, 1])
    if len(out) > LUT_BYTES:
        raise ValueError("waveform needs more than six groups")
    return bytes(out).ljust(LUT_BYTES, b"\0")


def sub_phases(table):
    for group in range(0, LUT_BYTES, 7):
        for byte in table[group + 1 : group + 5]:
            if byte & 0x3F:
                yield byte >> 6, byte & 0x3F


def net_drive(table):
    """Frames toward black minus frames toward white. VDHR shares VSH's polarity."""
    return sum(frames * (1 if level in (VSH, 3) else -1) for level, frames in sub_phases(table) if level != REST)


def total_frames(table):
    return sum(frames for _, frames in sub_phases(table))


def describe(table):
    return " ".join(f"{LEVEL_NAME[level]}{frames}" for level, frames in sub_phases(table)) or "-"


def mirror(table):
    """The same waveform with VSH and VSL swapped. Running it cancels the original's DC exactly."""
    if any(level == 3 for level, _ in sub_phases(table)):
        raise ValueError("VDHR has no mirror at the same voltage")
    out = bytearray(table)
    for group in range(0, LUT_BYTES, 7):
        for i in range(group + 1, group + 5):
            level = out[i] >> 6
            if level in (VSH, VSL):
                out[i] = (VSL if level == VSH else VSH) << 6 | (out[i] & 0x3F)
    return bytes(out)


def make_bank(ww, kw, wk, kk):
    """Five LUTs. VCOM spans the longest one: the datasheet fills a shorter LUT with 0 V."""
    return [lut((REST, max(map(total_frames, (ww, kw, wk, kk))))), ww, kw, wk, kk]


def rest_like(*tables):
    return lut((REST, max(map(total_frames, tables))))


def nudge(source, frames):
    """Land a pixel partway from a full rail, with no net drive.

    The first half pushes the way the pixel already sits, which moves nothing visible but pays for
    the second half. Without it every gray pixel keeps a DC offset.
    """
    return lut((VSL, frames), (VSH, frames)) if source == "white" else lut((VSH, frames), (VSL, frames))


def landing(frames, swing=16):
    """Land a gray from any start: all the way to black, then back by `frames`. Net drive is zero."""
    return lut((VSL, swing - frames), (VSH, swing), (VSL, frames))


def _recipe_lut(*groups):
    return bytes(b for group in groups for b in group).ljust(LUT_BYTES, b"\0")


# The bench recipe exactly as supplied, for reproducing it. Only Test 1 should run this: LUT 22 and 23
# carry a net drive, so `undo` has to follow.
_W4 = (1, 0x88, 0x88, 0x88, 0x88, 1, 1)
_K4 = (1, 0x48, 0x48, 0x48, 0x48, 1, 1)
_SETTLE = (1, 2, 2, 2, 2, 1, 1)
RECIPE_BANK = [
    _recipe_lut(*[(1, 8, 8, 8, 8, 1, 1)] * 3),
    _recipe_lut(_W4, _SETTLE, _K4),  # (1,1) black
    _recipe_lut(_W4),  # (0,1) light gray
    _recipe_lut((1, 0x88, 0x88, 0x88, 0x48, 1, 1)),  # (1,0) dark gray
    _recipe_lut(_K4, _SETTLE, _W4),  # (0,0) white
]

# Murphy production DEFAULT, for pushing a B/W base page.
PRODUCTION_BANK = [
    _recipe_lut((1, 0x0F, 0x0F, 0x0F, 1, 1, 1)),
    _recipe_lut((1, 0x4F, 0x8F, 0x0F, 1, 1, 1)),
    _recipe_lut((1, 0x0F, 0x8F, 0x0F, 1, 1, 1)),
    _recipe_lut((1, 0x4F, 0x8F, 0x4F, 1, 1, 1)),
    _recipe_lut((1, 0x0F, 0x8F, 0x4F, 1, 1, 1)),
]


class Frame:
    """Which LUT register each pixel takes."""

    def __init__(self, slot=WW):
        self.slots = bytearray([slot]) * (WIDTH * HEIGHT)

    def rect(self, x, y, w, h, slot):
        for row in range(y, min(y + h, HEIGHT)):
            start = row * WIDTH + x
            count = min(w, WIDTH - x)
            self.slots[start : start + count] = bytes([slot]) * count

    def planes(self):
        dtm1, dtm2 = bytearray(PLANE_BYTES), bytearray(PLANE_BYTES)
        for index, slot in enumerate(self.slots):
            bit1, bit2 = SLOT_PLANES[slot]
            mask = 0x80 >> (index & 7)
            if bit1:
                dtm1[index >> 3] |= mask
            if bit2:
                dtm2[index >> 3] |= mask
        return bytes(dtm1), bytes(dtm2)


class LabError(RuntimeError):
    pass


class SerialLink:
    """devctl.py's handling: default DTR/RTS on open, RTS dropped first on close, neither resets."""

    def __init__(self, port):
        import serial

        self.ser = serial.Serial(port, 115200, timeout=0.5)

    def write(self, data):
        self.ser.write(data)
        self.ser.flush()

    def readline(self):
        return self.ser.readline()

    def close(self):
        self.ser.rts = False
        self.ser.dtr = False
        self.ser.close()


class DryLink:
    """Answers like the firmware, without one."""

    def __init__(self):
        self.replies = []
        self.taking_frame = False

    def write(self, data):
        if self.taking_frame:
            self.taking_frame = False
            self.replies.append(b"OK sum=%d\n" % sum(data))
        elif data.startswith(b"FRAME"):
            self.taking_frame = True
            self.replies.append(b"READY\n")
        else:
            self.replies.append(b"OK dry\n")

    def readline(self):
        return self.replies.pop(0) if self.replies else b""

    def close(self):
        pass


class Lab:
    def __init__(self, link, state_dir, dry_run=False):
        self.link = link
        self.dry_run = dry_run
        self.state_dir = Path(state_dir)
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.journal_path = self.state_dir / "journal.json"
        self.refreshes = 0
        self.pending_line = b""

    def command(self, line, timeout=30):
        """Send one line. Returns (final OK text, the lines before it)."""
        self.link.write(line.encode() + b"\n")
        return self.read_reply(timeout)

    def read_line(self, deadline):
        """One whole line. A read timeout can split a line, so keep what arrived."""
        while time.time() < deadline:
            self.pending_line += self.link.readline()
            if self.pending_line.endswith(b"\n"):
                text = self.pending_line.decode(errors="replace").strip()
                self.pending_line = b""
                return text
        raise LabError("no reply from the device")

    def read_reply(self, timeout=30):
        extra = []
        deadline = time.time() + timeout
        while True:
            text = self.read_line(deadline)
            if text.startswith("OK"):
                return text[3:], extra
            if text.startswith("ERR"):
                raise LabError(text[4:])
            if text:
                extra.append(text)

    def arm(self):
        self.command("ARM")

    def clean(self, target):
        """Clean full white or black. Both planes match, so only WW or KK fire: no net drive."""
        reply, _ = self.command("W" if target == "white" else "K")
        self.refreshes += 1
        print(f"clean {target}: {reply}")

    def send_frame(self, dtm1, dtm2):
        self.link.write(b"FRAME\n")
        deadline = time.time() + 10
        while self.read_line(deadline) != "READY":
            pass
        self.link.write(dtm1 + dtm2)
        reply, _ = self.read_reply()
        if int(reply.split("=")[1]) != sum(dtm1) + sum(dtm2):
            raise LabError("frame checksum mismatch")

    def push(self, cfg, bank, frame, label, journal=True):
        """One refresh. Anything with a net drive goes in the journal for `undo`."""
        dtm1, dtm2 = frame.planes()
        self._send(cfg, bank, dtm1, dtm2, label)
        used = set(frame.slots)
        if journal and any(net_drive(bank[slot]) for slot in used):
            self.journal_add(cfg, bank, dtm1, dtm2, label)

    def _send(self, cfg, bank, dtm1, dtm2, label):
        print(f"{label}:")
        for name, slot in (("VCOM", VCOM), ("WW", WW), ("KW", KW), ("WK", WK), ("KK", KK)):
            print(f"  {name:4} net {net_drive(bank[slot]):+4}  {describe(bank[slot])}")
        self.command("CFG " + CONFIGS[cfg].hex())
        for slot, table in enumerate(bank):
            self.command(f"LUT {slot} {table.hex()}")
        self.send_frame(dtm1, dtm2)
        reply, _ = self.command("GO")
        self.refreshes += 1
        print(f"  {reply}")

    def journal(self):
        if self.journal_path.exists():
            return json.loads(self.journal_path.read_text())
        return []

    def journal_add(self, cfg, bank, dtm1, dtm2, label):
        if self.dry_run:
            return
        entries = self.journal()
        entries.append(
            {
                "cfg": cfg,
                "bank": [table.hex() for table in bank],
                "dtm1": base64.b64encode(dtm1).decode(),
                "dtm2": base64.b64encode(dtm2).decode(),
                "label": label,
            }
        )
        self.journal_path.write_text(json.dumps(entries))

    def undo(self):
        """Replay every journalled refresh mirrored, newest first, then leave the panel white."""
        entries = self.journal()
        if not entries:
            print("journal is empty: nothing to cancel")
        for entry in reversed(entries):
            bank = [mirror(bytes.fromhex(table)) for table in entry["bank"]]
            dtm1 = base64.b64decode(entry["dtm1"])
            dtm2 = base64.b64decode(entry["dtm2"])
            self._send(entry["cfg"], bank, dtm1, dtm2, "undo " + entry["label"])
        self.clean("white")
        if not self.dry_run and self.journal_path.exists():
            self.journal_path.unlink()


def photo(name, note=""):
    print(f"\nPHOTO {name}" + (f": {note}" if note else ""))


def show_marks(lab):
    """The frame cam.py finds the panel from. Production bank, so no net drive."""
    from cam import CALIB_MARKS, CALIB_ORIENT, CALIB_SIZE

    lab.arm()
    frame = Frame(WW)
    for x, y in (*CALIB_MARKS, CALIB_ORIENT):
        frame.rect(x, y, CALIB_SIZE, CALIB_SIZE, KK)
    lab.push("prod", PRODUCTION_BANK, frame, "calibration marks", journal=False)


def recipe_bands():
    """The recipe's four levels as horizontal bands: white, light, dark, black."""
    frame = Frame()
    band = HEIGHT // 4
    for index, slot in enumerate((KK, KW, WK, WW)):
        frame.rect(0, band * index, WIDTH, band, slot)
    return frame


def test1(lab, source, again=False):
    """One run of the recipe. A phone needs the frame to stay up, so each run is its own step."""
    lab.arm()
    if not again:
        lab.clean(source)
    run = 2 if again else 1
    lab.push("recipe", RECIPE_BANK, recipe_bands(), f"recipe bands from {source}, run {run}")
    photo(f"t1-from-{source}-run{run}", "top to bottom: white, light, dark, black")
    print("\nNext: `t1 %s --again`, or `undo` when this source is done." % source if not again else "\nNext: `undo`.")


def test1_slots(lab):
    """The same bytes in LUT 22 and 23 from the same start. Alternating 52-row stripes."""
    lab.arm()
    lab.clean("black")
    bank = list(RECIPE_BANK)
    bank[WK] = bank[KW]
    frame = Frame()
    for index in range(8):
        frame.rect(0, index * 52, WIDTH, 52, KW if index % 2 == 0 else WK)
    lab.push("recipe", bank, frame, "same bytes in KW and WK")
    photo("t1-slots", "the stripes should match if slots behave the same")
    print("\nRun `undo` next.")


WEDGE_STEPS = (1, 2, 3, 4, 6, 8, 12, 16)
WEDGE_BAND = HEIGHT // len(WEDGE_STEPS)
WEDGE_BANDS_WIDTH = 200  # the rest of the width is the opposite-rail reference


def test2_wedge(lab, source, cfg, steps=WEDGE_STEPS):
    """A step wedge: band i gets steps[i] frames. Three refreshes, four LUTs each, one photograph."""
    lab.arm()
    lab.clean(source)
    reference = nudge(source, 16)
    batches = [[(0, KW), (1, WK)], [(2, KW), (3, WK), (4, KK)], [(5, KW), (6, WK), (7, KK)]]
    for number, batch in enumerate(batches):
        tables = {slot: nudge(source, steps[index]) for index, slot in batch}
        if number == 0:
            tables[KK] = reference
        idle = rest_like(*tables.values())
        bank = make_bank(idle, tables.get(KW, idle), tables.get(WK, idle), tables.get(KK, idle))
        frame = Frame(WW)
        for index, slot in batch:
            frame.rect(0, index * WEDGE_BAND, WEDGE_BANDS_WIDTH, WEDGE_BAND, slot)
        if number == 0:
            frame.rect(WEDGE_BANDS_WIDTH, 0, WIDTH - WEDGE_BANDS_WIDTH, HEIGHT, KK)
        lab.push(cfg, bank, frame, f"wedge from {source}, {cfg}, batch {number + 1}")
    photo(f"t2-{source}-{cfg}", f"bands top to bottom: {steps} frames; right strip is the opposite rail")


def base_frame(black_pixels):
    """A B/W page for the production bank: same data in both planes, so WW or KK."""
    frame = Frame(WW)
    for index, black in enumerate(black_pixels):
        if black:
            frame.slots[index] = KK
    return frame


def overlay_bank(light, dark, light_b=None):
    """White-base pixels go light through KW, black-base pixels go dark through WK, the rest idle.

    `light_b` is a second light level on the spare KK slot, for comparing two in one refresh.
    """
    light_table, dark_table = nudge("white", light), nudge("black", dark)
    tables = [light_table, dark_table]
    if light_b is not None:
        tables.append(nudge("white", light_b))
    idle = rest_like(*tables)
    return make_bank(idle, light_table, dark_table, tables[2] if light_b is not None else idle)


def show_overlay(lab, levels, light, dark, cfg, label, light_b=None, turned=False):
    """Levels 0-3 (white to black) as a B/W base, then one nudge. Same flow as the X3.

    With `light_b`, the lower half as seen uses it. `turned` says the page was rotated, which puts
    that half at the start of the frame.
    """
    base = base_frame([level >= 2 for level in levels])
    lab.push("prod", PRODUCTION_BANK, base, f"{label}: B/W base", journal=False)
    frame = Frame(WW)
    half = len(levels) // 2
    for index, level in enumerate(levels):
        if level == 1:
            frame.slots[index] = KK if light_b is not None and (index < half) == turned else KW
        elif level == 2:
            frame.slots[index] = WK
    lab.push(cfg, overlay_bank(light, dark, light_b), frame, f"{label}: nudge")


def show_onepass(lab, levels, light, dark, cfg, label):
    """Levels 0-3 in one refresh, each pixel reset to a rail first, so the start does not matter."""
    bank = make_bank(lut((VSH, 16), (VSL, 16)), landing(light), landing(dark), lut((VSL, 16), (VSH, 16)))
    frame = Frame(WW)
    for index, level in enumerate(levels):
        frame.slots[index] = (WW, KW, WK, KK)[level]
    lab.push(cfg, bank, frame, label)


def fine_levels():
    """Gray at pixel scale: 1, 2 and 4 px checkers and lines, on white above and on black below."""
    levels = bytearray(WIDTH * HEIGHT)
    half = HEIGHT // 2
    for y in range(half, HEIGHT):
        levels[y * WIDTH : (y + 1) * WIDTH] = bytes([3]) * WIDTH
    for top, gray in ((0, 1), (half, 2)):
        for index, scale in enumerate((1, 2, 4)):
            row0 = top + index * 64 + 8
            for y in range(row0, row0 + 48):
                for x in range(8, 112):  # checker
                    if ((x // scale) + (y // scale)) % 2:
                        levels[y * WIDTH + x] = gray
                for x in range(128, 232):  # vertical lines
                    if (x // scale) % 2:
                        levels[y * WIDTH + x] = gray
    return levels


SAMPLE_TEXT = (
    "The quick brown fox jumps over the lazy dog. Sphinx of black quartz, judge my vow. "
    "Pack my box with five dozen liquor jugs. How vexingly quick daft zebras jump! "
    "The five boxing wizards jump quickly. Bright vixens jump; dozy fowl quack."
)


def text_levels(font_path, size, rotate=False, split=False, keep_gray=False):
    """A text page as levels 0-3, using FreeType's antialiasing through Pillow.

    With `split`, the same text fills both halves: four levels above, 1-bit below, or four levels in
    both with `keep_gray`. Two refreshes can only be compared from memory, and this puts them side
    by side.
    """
    from PIL import Image, ImageDraw, ImageFont

    font = ImageFont.truetype(font_path, size)
    image = Image.new("L", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    half = HEIGHT // 2
    bottom = half if split else HEIGHT
    y, line = 6, ""
    for word in SAMPLE_TEXT.split() * 3:
        trial = f"{line} {word}".strip()
        if draw.textlength(trial, font=font) > WIDTH - 16:
            draw.text((8, y), line, font=font, fill=255)
            y += int(size * 1.35)
            line = word
        else:
            line = trial
        if y > bottom - size * 1.6:
            break
    levels = bytearray(min(value // 64, 3) for value in image.tobytes())
    if split:
        upper = levels[: half * WIDTH]
        levels[half * WIDTH :] = upper if keep_gray else bytes(3 if level >= 2 else 0 for level in upper)
    if rotate:
        levels.reverse()
    return levels


def show_page(lab, levels, args, label):
    if args.mode == "bw":
        lab.push("prod", PRODUCTION_BANK, base_frame([level >= 2 for level in levels]), label, journal=False)
        return
    if args.mode == "swapped":  # the plane mix-up the findings describe
        levels = bytearray((0, 2, 1, 3)[level] for level in levels)
    if args.mode == "onepass":
        show_onepass(lab, levels, args.light, args.dark, args.cfg, label)
    else:
        light_b = getattr(args, "light_b", None)
        show_overlay(lab, levels, args.light, args.dark, args.cfg, label, light_b, getattr(args, "rotate", False))


FRAME_RATES = [*range(5, 85, 5), *range(85, 125, 5), *range(130, 201, 10)]
TR_BYTES = 0xE9
TR_FIRST = 0x2A
TR_COUNT = 17
TR_LUTS = (("VCOM", 0x09), ("WW", 0x33), ("KW", 0x5D), ("WK", 0x87), ("KK", 0xB1))


def uniform(raw):
    return len(set(raw)) <= 1


def parse_otp(raw):
    """A readable account of an OTP image, per the UC8253c address map (datasheet pp.48-53)."""
    out = []
    bases = [base for base in (0x0000, 0x1000) if len(raw) > base and raw[base] == 0xA5]
    if not bases:
        return f"no bank carries the 0xA5 check code; first bytes {raw[:16].hex()}"
    for base in bases:
        bank = raw[base : base + 0x1000]
        out.append(f"bank at {base:#06x}")
        out.append(f"  defaults enable key {bank[1]:#04x}, PSR {bank[2]:#04x}, CDI {bank[8]:#04x}, TSSET {bank[0x13]:#04x}")
        out.append(f"  chip id {bank[0x14:0x17].hex()}, LUT version {bank[0x17:0x1A].hex()}")
        out.append(f"  temperature bounds {[b - 256 if b > 127 else b for b in bank[0x1A:0x2A]]}")
        for index in range(TR_COUNT):
            tr = bank[TR_FIRST + index * TR_BYTES : TR_FIRST + (index + 1) * TR_BYTES]
            if uniform(tr):
                continue
            rate = FRAME_RATES[tr[0] & 0x1F]
            vsh, vsl, vdhr = (2.4 + 0.2 * (value & 0x3F) for value in tr[2:5])
            vcom = -0.1 - 0.1 * (tr[5] & 0x7F)
            out.append(
                f"  TR{index}: {rate} Hz, VSH {vsh:.1f}, VSL -{vsl:.1f}, VDHR {vdhr:.1f}, VCOM {vcom:.1f}, "
                f"opt {tr[6]:#04x}, xon {tr[7:9].hex()}"
            )
            tables = {name: bytes(tr[offset : offset + LUT_BYTES]) for name, offset in TR_LUTS}
            for name, table in tables.items():
                out.append(f"    {name:4} net {net_drive(table):+4} frames {total_frames(table):3}  {describe(table)}")
            if len({tables[name] for name in ("WW", "KW", "WK", "KK")}) == 4:
                out.append("    all four transition LUTs differ: read this one for gray behaviour")
    return "\n".join(out)


def read_info(lab):
    """Nothing here refreshes the panel."""
    reply, _ = lab.command("HELLO")
    print(reply)
    _, lines = lab.command("REV", timeout=10)
    mode = None
    for line in lines:
        print(line)
        fields = line.split()
        if fields[0] == "REV" and mode is None and bytes.fromhex(fields[2])[1:2] == b"\x09":
            mode = int(fields[1])
    if mode is None:
        print("No read mode returned CHIP_REV 0x09, so OTP data would not be trustworthy. Stopping.")
        return None
    print(f"read mode {mode} returned CHIP_REV 0x09")
    for power_on in (0, 1):
        _, lines = lab.command(f"OTP {mode} {power_on}", timeout=60)
        raw = bytes.fromhex("".join(line.split()[2] for line in lines if line.startswith("OTP ")))
        if not uniform(raw):
            break
        print(f"OTP read with power_on={power_on} came back uniform")
    path = lab.state_dir / f"murphy-otp-{time.strftime('%Y%m%d-%H%M%S')}.bin"
    path.write_bytes(raw)
    print(f"saved {len(raw)} bytes to {path}")
    print(parse_otp(raw))
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=os.environ.get("MURPHY_PORT"), help="serial port, or set MURPHY_PORT")
    parser.add_argument("--state", default="~/.local/state/murphy-gray-lab", help="journal and OTP dumps")
    parser.add_argument("--dry-run", action="store_true")
    steps = parser.add_subparsers(dest="step", required=True)

    steps.add_parser("info", help="Test 0: revision and OTP, no refresh")
    otp = steps.add_parser("parse-otp", help="print a saved OTP image")
    otp.add_argument("file")
    steps.add_parser("white", help="clean white")
    steps.add_parser("black", help="clean black")
    steps.add_parser("undo", help="cancel journalled drive, then clean white")
    steps.add_parser("marks", help="calibration marks for cam.py")
    t1 = steps.add_parser("t1", help="Test 1: the recipe from a known start")
    t1.add_argument("source", choices=("white", "black"))
    t1.add_argument("--again", action="store_true", help="run on top of what is there, no clean first")
    steps.add_parser("t1-slots", help="Test 1: same bytes in LUT 22 and 23")
    t2 = steps.add_parser("t2", help="Test 2: frame-count wedge")
    t2.add_argument("source", choices=("white", "black"))
    t2.add_argument("--cfg", choices=sorted(CONFIGS), default="prod")
    for name, help_text in (("fine", "Test 3: pixel-scale patterns"), ("text", "Test 3: a text page")):
        page = steps.add_parser(name, help=help_text)
        page.add_argument("--light", type=int, required=True, help="frames that land light gray")
        page.add_argument("--dark", type=int, required=True, help="frames that land dark gray")
        page.add_argument("--cfg", choices=sorted(CONFIGS), default="prod")
        page.add_argument("--mode", choices=("overlay", "onepass", "swapped", "bw"), default="overlay")
        if name == "text":
            page.add_argument("--font", required=True)
            page.add_argument("--size", type=int, default=16)
            page.add_argument("--rotate", action="store_true", help="turn the page 180 degrees")
            page.add_argument("--split", action="store_true", help="four levels in one half, 1-bit in the other")
            page.add_argument("--light-b", type=int, help="overlay: a second light level for the lower half")
    args = parser.parse_args()

    if args.step == "parse-otp":
        print(parse_otp(Path(args.file).read_bytes()))
        return

    if args.dry_run:
        link = DryLink()
    elif args.port:
        link = SerialLink(args.port)
    else:
        sys.exit("give --port or set MURPHY_PORT")
    lab = Lab(link, os.path.expanduser(args.state), args.dry_run)

    try:
        if args.step == "info":
            read_info(lab)
        elif args.step in ("white", "black"):
            lab.arm()
            lab.clean(args.step)
        elif args.step == "marks":
            show_marks(lab)
        elif args.step == "undo":
            lab.arm()
            lab.undo()
        elif args.step == "t1":
            test1(lab, args.source, args.again)
        elif args.step == "t1-slots":
            test1_slots(lab)
        elif args.step == "t2":
            test2_wedge(lab, args.source, args.cfg)
        elif args.step in ("fine", "text"):
            lab.arm()
            if args.step == "fine":
                levels = fine_levels()
            else:
                compare = args.light_b is not None
                levels = text_levels(args.font, args.size, args.rotate, args.split or compare, compare)
            show_page(lab, levels, args, f"{args.step} {args.mode}")
            photo(f"t3-{args.step}-{args.mode}-l{args.light}-d{args.dark}")
    except LabError as error:
        sys.exit(f"STOP: {error}")
    finally:
        print(f"\nrefreshes this step: {lab.refreshes}; journal entries: {len(lab.journal())}")
        link.close()


if __name__ == "__main__":
    main()
