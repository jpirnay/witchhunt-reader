#!/usr/bin/env python3
"""Runs lab.py against the firmware built for the PC (test/host/fake_device.cpp).

The point is what can be proven without the panel: the bytes on the bus. `W` is here because the
harness this lab replaced had a white recovery that painted the panel black, and its tests passed.
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import cam  # noqa: E402
import lab  # noqa: E402

FAKE = os.environ.get("FAKE_DEVICE", "")
OTP_WRITE_COMMANDS = {"a0", "a1", "a3"}


class ProcessLink:
    def __init__(self, env):
        self.process = subprocess.Popen([FAKE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env={**os.environ, **env})

    def write(self, data):
        self.process.stdin.write(data)
        self.process.stdin.flush()

    def readline(self):
        return self.process.stdout.readline()

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)


class Bench(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.log = Path(self.tmp.name) / "bus.log"
        self.open_lab()

    def open_lab(self, **env):
        self.link = ProcessLink({"FAKE_BUS_LOG": str(self.log), **env})
        self.lab = lab.Lab(self.link, self.tmp.name)
        self.closed = False
        self.addCleanup(self.close)

    def close(self):
        if not self.closed:
            self.closed = True
            self.link.close()

    def bus(self):
        """(command, length, first byte, uniform) for every register write."""
        self.close()
        rows = [line.split() for line in self.log.read_text().splitlines()]
        return [(cmd, int(length), first, same == "1") for cmd, length, first, same in rows]

    def assert_refresh_order(self, bus):
        order = [row[0] for row in bus]
        expected = ["20", "21", "22", "23", "24", "10", "13", "12", "02"]
        self.assertEqual([c for c in order if c in expected], expected)
        self.assertLess(order.index("04"), order.index("20"))


class CleanRefresh(Bench):
    def test_white_is_ff_in_both_planes(self):
        self.lab.arm()
        self.lab.clean("white")
        bus = self.bus()
        self.assert_refresh_order(bus)
        planes = [row for row in bus if row[0] in ("10", "13")]
        self.assertEqual(planes, [("10", lab.PLANE_BYTES, "ff", True), ("13", lab.PLANE_BYTES, "ff", True)])

    def test_black_is_00_in_both_planes(self):
        self.lab.arm()
        self.lab.clean("black")
        planes = [row for row in self.bus() if row[0] in ("10", "13")]
        self.assertEqual(planes, [("10", lab.PLANE_BYTES, "00", True), ("13", lab.PLANE_BYTES, "00", True)])

    def test_production_lut_lengths_and_init(self):
        self.lab.arm()
        self.lab.clean("white")
        bus = {row[0]: row for row in self.bus()}
        self.assertEqual([bus[c][1] for c in ("20", "21", "22", "23", "24")], [56, 42, 56, 42, 42])
        self.assertEqual(bus["82"][2], "0f")
        self.assertEqual(bus["50"][2], "97")

    def test_refuses_to_drive_before_arm(self):
        for command in ("W", "K", "GO"):
            with self.assertRaisesRegex(lab.LabError, "not armed"):
                self.lab.command(command)
        self.assertEqual(self.bus(), [])


class Rules(Bench):
    def setUp(self):
        super().setUp()
        self.lab.arm()

    def refused(self, line, reason):
        with self.assertRaisesRegex(lab.LabError, reason):
            self.lab.command(line)

    def test_unbalanced_lut_above_the_limit(self):
        self.refused(f"LUT 1 {lab.lut((lab.VSH, 33)).hex()}", "net drive")
        self.lab.command(f"LUT 1 {lab.lut((lab.VSH, 32)).hex()}")

    def test_too_long(self):
        self.refused(f"LUT 1 {lab.lut((lab.REST, 97)).hex()}", "too many frames")

    def test_vcom_lut_stays_at_vcom_dc(self):
        self.refused(f"LUT 0 {lab.lut((lab.VSH, 4)).hex()}", "VCOM LUT")

    def test_repeat_bytes(self):
        table = bytearray(lab.lut((lab.VSH, 4)))
        table[0] = 4  # the bench's "0x04 broke drive"
        self.refused(f"LUT 1 {table.hex()}", "repeat byte")

    def test_config_outside_the_known_settings(self):
        self.refused("CFG 000d0fd709", "VSL")
        self.refused("CFG 3b0d13d709", "VCOM")
        self.refused("CFG 3b0d0fd70a", "PLL")
        for name, config in lab.CONFIGS.items():
            with self.subTest(name):
                self.lab.command("CFG " + config.hex())

    def test_go_needs_everything_loaded(self):
        self.refused("GO", "LUTs not loaded")


class Waveforms(unittest.TestCase):
    def test_nudges_are_balanced_and_end_the_right_way(self):
        for frames in lab.WEDGE_STEPS:
            self.assertEqual(lab.net_drive(lab.nudge("white", frames)), 0)
            self.assertEqual(lab.net_drive(lab.nudge("black", frames)), 0)
        self.assertEqual(lab.describe(lab.nudge("white", 3)), "VSL3 VSH3")
        self.assertEqual(lab.describe(lab.nudge("black", 3)), "VSH3 VSL3")

    def test_landing_is_balanced(self):
        for frames in range(1, 16):
            self.assertEqual(lab.net_drive(lab.landing(frames)), 0)

    def test_mirror_cancels_every_recipe_lut(self):
        for table in lab.RECIPE_BANK:
            self.assertEqual(lab.net_drive(table) + lab.net_drive(lab.mirror(table)), 0)
            self.assertEqual(lab.mirror(lab.mirror(table)), table)

    def test_recipe_matches_the_supplied_bytes(self):
        self.assertEqual(lab.net_drive(lab.RECIPE_BANK[lab.KW]), -32)
        self.assertEqual(lab.net_drive(lab.RECIPE_BANK[lab.WK]), -16)
        self.assertEqual(lab.describe(lab.RECIPE_BANK[lab.KK]), "VSH8 VSH8 VSH8 VSH8 rest2 rest2 rest2 rest2 VSL8 VSL8 VSL8 VSL8")

    def test_plane_bits_select_the_datasheet_slots(self):
        frame = lab.Frame(lab.WW)
        frame.slots[0], frame.slots[1], frame.slots[2], frame.slots[3] = lab.WW, lab.KW, lab.WK, lab.KK
        dtm1, dtm2 = frame.planes()
        self.assertEqual(dtm1[0] >> 4, 0b1010)
        self.assertEqual(dtm2[0] >> 4, 0b1100)

    def test_vcom_covers_the_longest_lut(self):
        bank = lab.make_bank(lab.lut((lab.REST, 4)), lab.nudge("white", 8), lab.lut((lab.REST, 4)), lab.lut((lab.REST, 4)))
        self.assertEqual(lab.total_frames(bank[lab.VCOM]), 16)


class Steps(Bench):
    def test_wedge_is_balanced_so_nothing_needs_undoing(self):
        lab.test2_wedge(self.lab, "white", "prod")
        self.assertEqual(self.lab.refreshes, 4)
        self.assertEqual(self.lab.journal(), [])
        self.assertEqual([row[0] for row in self.bus()].count("12"), 4)

    def test_recipe_is_journalled_and_undo_mirrors_it(self):
        lab.test1(self.lab, "black")
        lab.test1(self.lab, "black", again=True)
        self.assertEqual(len(self.lab.journal()), 2)
        self.lab.undo()
        self.assertEqual(self.lab.journal(), [])
        self.assertEqual(self.lab.refreshes, 1 + 2 + 2 + 1)

    def test_overlay_page_is_base_then_nudge_and_balanced(self):
        self.lab.arm()
        levels = bytearray([0, 1, 2, 3]) * (lab.WIDTH * lab.HEIGHT // 4)
        lab.show_overlay(self.lab, levels, 2, 3, "prod", "page")
        self.assertEqual(self.lab.refreshes, 2)
        self.assertEqual(self.lab.journal(), [])

    def test_two_light_levels_share_one_nudge(self):
        self.lab.arm()
        pixels = lab.WIDTH * lab.HEIGHT
        for turned, first_half_slot in ((False, lab.KW), (True, lab.KK)):
            bank = lab.overlay_bank(1, 3, 2)
            self.assertEqual(lab.describe(bank[lab.KK]), "VSL2 VSH2")
            pushed = []
            self.lab.push = lambda cfg, bank, frame, label, journal=True: pushed.append(frame)
            lab.show_overlay(self.lab, bytearray([1]) * pixels, 1, 3, "prod", "page", 2, turned)
            self.assertEqual(pushed[-1].slots[0], first_half_slot)
            self.assertNotEqual(pushed[-1].slots[-1], first_half_slot)

    def test_onepass_is_one_refresh_and_balanced(self):
        self.lab.arm()
        levels = bytearray([0, 1, 2, 3]) * (lab.WIDTH * lab.HEIGHT // 4)
        lab.show_onepass(self.lab, levels, 6, 3, "prod", "page")
        self.assertEqual(self.lab.refreshes, 1)
        self.assertEqual(self.lab.journal(), [])

    def test_fine_pattern_has_all_the_cases(self):
        levels = lab.fine_levels()
        self.assertEqual(set(levels), {0, 1, 2, 3})

    def test_no_step_can_reach_the_otp_write_commands(self):
        lab.test1(self.lab, "white")
        lab.test2_wedge(self.lab, "black", "oem-alt")
        lab.read_info(self.lab)
        commands = {row[0] for row in self.bus()}
        self.assertFalse(commands & OTP_WRITE_COMMANDS)


class Camera(unittest.TestCase):
    """The panel sits upside down in front of the webcam, so that case has to keep working."""

    def marks_image(self, place):
        from PIL import Image, ImageDraw

        white = Image.new("L", (cam.CAM_W, cam.CAM_H), 200)
        marks = white.copy()
        draw = ImageDraw.Draw(marks)
        for mark in (*cam.CALIB_MARKS, cam.CALIB_ORIENT):
            x0, y0 = place(mark[0], mark[1])
            x1, y1 = place(mark[0] + cam.CALIB_SIZE, mark[1] + cam.CALIB_SIZE)
            draw.rectangle((min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)), fill=40)
        return white, marks

    def assert_maps(self, place):
        white, marks = self.marks_image(place)
        camera = cam.calibrate(white, marks)
        a, b, c, d, e, f, g, h = camera["coefficients"]
        for x, y in ((0, 0), (240, 0), (0, 416), (120, 208)):
            scale = g * x + h * y + 1
            got = ((a * x + b * y + c) / scale, (d * x + e * y + f) / scale)
            want = place(x, y)
            self.assertAlmostEqual(got[0], want[0], delta=8)
            self.assertAlmostEqual(got[1], want[1], delta=8)

    def test_upright(self):
        self.assert_maps(lambda x, y: (600 + 2.2 * x, 100 + 2.2 * y))

    def test_upside_down(self):
        self.assert_maps(lambda x, y: (1200 - 2.2 * x, 1000 - 2.2 * y))


class TextPage(unittest.TestCase):
    FONT = HERE.parents[2] / "lib/EpdFont/builtinFonts/source/Bookerly/Bookerly-Regular.ttf"

    def test_split_has_grays_in_one_half_only(self):
        levels = lab.text_levels(str(self.FONT), 16, split=True)
        half = lab.WIDTH * lab.HEIGHT // 2
        self.assertEqual(set(levels[:half]), {0, 1, 2, 3})
        self.assertEqual(set(levels[half:]), {0, 3})

    def test_rotate_turns_the_page(self):
        plain = lab.text_levels(str(self.FONT), 16, split=True)
        turned = lab.text_levels(str(self.FONT), 16, rotate=True, split=True)
        self.assertEqual(bytes(plain), bytes(reversed(turned)))


class FastPageTurn(unittest.TestCase):
    def slots(self, old, new, aware):
        frame = lab.fast_frame(bytearray([old]) * (lab.WIDTH * lab.HEIGHT), bytearray([new]) * (lab.WIDTH * lab.HEIGHT), aware)
        return frame.slots[0]

    def test_plain_fast_only_sees_black_and_white(self):
        self.assertEqual(self.slots(1, 0, False), lab.WW)  # gray stays: this is the ghost
        self.assertEqual(self.slots(2, 3, False), lab.KK)
        self.assertEqual(self.slots(0, 3, False), lab.WK)
        self.assertEqual(self.slots(3, 0, False), lab.KW)

    def test_gray_aware_drives_gray_to_the_new_rail(self):
        self.assertEqual(self.slots(1, 0, True), lab.KW)
        self.assertEqual(self.slots(2, 0, True), lab.KW)
        self.assertEqual(self.slots(1, 3, True), lab.WK)
        self.assertEqual(self.slots(2, 3, True), lab.WK)

    def test_gray_aware_leaves_clean_pixels_alone(self):
        self.assertEqual(self.slots(0, 0, True), lab.WW)
        self.assertEqual(self.slots(3, 3, True), lab.KK)

    def test_fast_bank_matches_production(self):
        self.assertEqual(lab.describe(lab.FAST_BANK[lab.KW]), "VSL15 rest1")
        self.assertEqual(lab.describe(lab.FAST_BANK[lab.WK]), "VSH15 rest1")
        self.assertEqual(lab.describe(lab.FAST_BANK[lab.KK]), "rest15 VSH1")


class SplitLink:
    """A serial port whose read timeout fired in the middle of a line."""

    def __init__(self, chunks):
        self.chunks = list(chunks)

    def write(self, data):
        pass

    def readline(self):
        return self.chunks.pop(0) if self.chunks else b""


class Links(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_a_split_line_is_joined(self):
        link = SplitLink([b"OTP 0000 ab", b"", b"cd\n", b"O", b"K otp\n"])
        reply, lines = lab.Lab(link, self.tmp.name).command("OTP 0 0")
        self.assertEqual((reply, lines), ("otp", ["OTP 0000 abcd"]))

    def test_dry_run_gets_through_every_step_without_a_journal(self):
        dry = lab.Lab(lab.DryLink(), self.tmp.name, dry_run=True)
        levels = bytearray([0, 1, 2, 3]) * (lab.WIDTH * lab.HEIGHT // 4)
        lab.test1(dry, "white")
        lab.test1(dry, "white", again=True)
        lab.test1_slots(dry)
        lab.test2_wedge(dry, "black", "oem-alt")
        lab.show_overlay(dry, levels, 2, 3, "prod", "page")
        lab.show_onepass(dry, levels, 6, 3, "prod", "page")
        dry.undo()
        self.assertEqual(dry.refreshes, 2 + 1 + 1 + 1 + 4 + 2 + 1 + 1)
        self.assertEqual(dry.journal(), [])


class Failure(Bench):
    def test_busy_timeout_stops_the_step_and_is_named(self):
        self.close()
        self.open_lab(FAKE_STUCK="1")
        self.lab.arm()
        with self.assertRaisesRegex(lab.LabError, "refresh"):
            self.lab.clean("white")

    def test_frame_is_checked_before_go(self):
        self.lab.arm()
        frame = lab.Frame()
        dtm1, dtm2 = frame.planes()
        self.lab.send_frame(dtm1, dtm2)
        self.lab.command("HELLO")


def otp_image():
    image = bytearray(b"\xff" * 8192)
    image[0] = 0xA5
    image[0x14:0x17] = bytes.fromhex("123456")
    image[0x17:0x1A] = bytes.fromhex("0a0b0c")
    image[0x1A:0x2A] = bytes(range(16))
    tr = bytearray(lab.TR_BYTES)
    tr[0:6] = bytes([0x09, 0x10, 0x3F, 0x3F, 0x0D, 0x0F])
    for offset, table in zip((0x33, 0x5D, 0x87, 0xB1), (lab.nudge("white", n) for n in (1, 2, 3, 4))):
        tr[offset : offset + 42] = table
    image[lab.TR_FIRST : lab.TR_FIRST + lab.TR_BYTES] = tr
    return bytes(image)


class Otp(Bench):
    def test_dump_is_saved_and_parsed(self):
        self.close()
        path = Path(self.tmp.name) / "otp.bin"
        path.write_bytes(otp_image())
        self.open_lab(FAKE_OTP=str(path))
        raw = lab.read_info(self.lab)
        self.assertEqual(raw, otp_image())
        text = lab.parse_otp(raw)
        self.assertIn("TR0: 50 Hz, VSH 15.0, VSL -15.0, VDHR 5.0, VCOM -1.6", text)
        self.assertIn("all four transition LUTs differ", text)

    def test_unprogrammed_otp_is_reported_not_parsed(self):
        raw = lab.read_info(self.lab)
        self.assertIn("no bank carries", lab.parse_otp(raw))


if __name__ == "__main__":
    unittest.main()
