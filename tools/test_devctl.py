#!/usr/bin/env python3
"""Host tests of tools/devctl.py against a model of the device's port: no pyserial and no device needed. They cover
what a run does when the port goes away between or during steps, which reply a step takes, how long it waits for
it, and that a reply still to come ends the run. Run by ctest (test/devctl)."""
import importlib
import io
import os
import sys
import tempfile
import time
import types
import unittest
from contextlib import redirect_stdout


class PortGone(Exception):
    """pyserial's SerialException, as a port that went away raises it on a write."""


class FakeDevice:
    """The device behind the port. `answers` maps a command (the text after CMD:) to the lines it answers with, in
    order: a line at once, or (seconds, line) for one that comes that much later; bytes go out as they are (a
    screenshot's frame). Every opening of the port is a FakePort; output goes to the newest one, as the device's USB
    port is one."""

    def __init__(self, answers):
        self.answers = answers
        self.opened = []
        self.received = []  # (port number, command) for every CMD: line the device took
        self.after_command = None  # called with the port once a command is answered
        self.later = []  # (due time, line) still to come

    def deliver(self):
        """The lines that are due, in turn, to the port open now."""
        now = time.time()
        while self.later and self.later[0][0] <= now:
            out = self.later.pop(0)[1]
            self.opened[-1].incoming += out if isinstance(out, bytes) else (out + "\n").encode()

    def open(self):
        port = FakePort(self, len(self.opened))
        self.opened.append(port)
        return port

    def take(self, port, data):
        port.partial += data
        while b"\n" in port.partial:
            line, port.partial = port.partial.split(b"\n", 1)
            text = line.decode().rstrip("\r")
            if not text.startswith("CMD:"):
                continue  # the blank line devctl writes after opening the port
            command = text[len("CMD:"):]
            self.received.append((port.number, command))
            for answer in self.answers.get(command, ()):
                delay, line = answer if isinstance(answer, tuple) else (0, answer)
                # The device answers its commands in turn: a line waits for the ones due before it.
                due = max([time.time() + delay] + [d for d, _ in self.later])
                self.later.append((due, line))
            self.deliver()
            if self.after_command:
                self.after_command(port)


class FakePort:
    def __init__(self, device, number):
        self.device = device
        self.number = number
        self.is_open = True
        self.lines = {"rts": True, "dtr": True}
        self.incoming = bytearray()
        self.partial = b""
        self.gone = False  # the device rebooted: every call fails, as on a port that went away
        self.fail_write = False  # it goes away during the next write

    def _set_line(self, name, value):
        if self.gone:
            raise PortGone(f"could not set {name}: [Errno 5] Input/output error")
        self.lines[name] = value

    rts = property(lambda self: self.lines["rts"], lambda self, value: self._set_line("rts", value))
    dtr = property(lambda self: self.lines["dtr"], lambda self, value: self._set_line("dtr", value))

    @property
    def in_waiting(self):
        if self.gone:
            raise OSError(5, "Input/output error")  # pyserial's in_waiting lets the ioctl's error through
        self.device.deliver()
        return len(self.incoming)

    def read(self, size):
        if self.gone:
            raise PortGone("device reports readiness to read but returned no data")
        self.device.deliver()
        if not self.incoming:
            time.sleep(0.01)  # the port's read timeout, shortened
            return b""
        data = bytes(self.incoming[:size])
        del self.incoming[:size]
        return data

    def write(self, data):
        if self.gone or self.fail_write:
            self.gone = True
            raise PortGone("write failed: [Errno 5] Input/output error")
        self.device.take(self, bytes(data))
        return len(data)

    def reset_input_buffer(self):
        self.incoming.clear()

    def close(self):
        self.is_open = False


class Harness:
    device = None


def load_devctl():
    """tools/devctl.py with the model in pyserial's place. The fake `serial` module is in sys.modules only while
    devctl imports it (devctl keeps its own reference), so nothing else that runs in this process sees it."""
    fake_serial = types.ModuleType("serial")
    fake_serial.SerialException = PortGone
    fake_serial.Serial = lambda *args, **kwargs: Harness.device.open()
    saved = {name: sys.modules.get(name) for name in ("serial", "devctl")}
    folder = os.path.dirname(os.path.abspath(__file__))
    sys.modules["serial"] = fake_serial
    sys.path.insert(0, folder)
    try:
        return importlib.import_module("devctl")
    finally:
        sys.path.remove(folder)
        for name, module in saved.items():
            if module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module


devctl = load_devctl()
devctl.REOPEN_PAUSE = 0
devctl.REPLY_TIMEOUT = 0.3  # the device reads a command and answers it within this, here
LATE = 0.6  # a reply that comes after the host gave up on it


def connect(device):
    Harness.device = device
    return devctl.Device("/dev/fake")


def run(port, steps):
    out = io.StringIO()
    with redirect_stdout(out):
        ok = devctl.run(port, steps, rotate=0, keep_going=True)
    return ok, out.getvalue()


class PortGoesAway(unittest.TestCase):
    def test_a_write_the_port_lost_fails_its_step_and_is_not_sent_again(self):
        device = FakeDevice({"BTN:RIGHT": ["BTN_OK"], "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        device.opened[0].fail_write = True  # the device restarts while the command is written
        ok, out = run(port, ["cmd:BTN:RIGHT", "state"])
        self.assertFalse(ok)
        self.assertIn("DELIVERY UNCERTAIN: CMD:BTN:RIGHT (port reopened, not resent)", out)
        self.assertIn("step failed: cmd:BTN:RIGHT", out)
        # The next step runs on the reopened port, and the press is not replayed there.
        self.assertEqual(device.received, [(1, "STATE")])
        self.assertIn("STATE:activity=Home", out)
        self.assertNotIn("step failed: state", out)

    def test_a_port_lost_between_steps_is_reopened_before_the_next_command(self):
        device = FakeDevice({"BTN:RIGHT": ["BTN_OK"], "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        ok, _ = run(port, ["cmd:BTN:RIGHT"])
        self.assertTrue(ok)
        device.opened[0].gone = True  # the device went away while the host was between steps
        ok, out = run(port, ["state"])
        self.assertTrue(ok, out)
        self.assertEqual(device.received, [(0, "BTN:RIGHT"), (1, "STATE")])

    def test_an_upload_whose_piece_is_lost_is_not_sent_again(self):
        with tempfile.TemporaryDirectory() as folder:
            local = os.path.join(folder, "a.bin")
            with open(local, "wb") as f:
                f.write(b"hello")  # CRC32 3610a686
            device = FakeDevice({"FS:PUT:/test/a.bin:5:3610a686": ["FS_PUT_READY:4096"]})
            port = connect(device)
            device.after_command = lambda p: setattr(p, "fail_write", True)  # gone before the piece
            ok, out = run(port, [f"put:{local}:/test/a.bin"])
        self.assertFalse(ok)
        self.assertIn("DELIVERY UNCERTAIN: FS:PUT:/test/a.bin, the piece at byte 0 (port reopened, not resent)", out)
        self.assertEqual(device.received, [(0, "FS:PUT:/test/a.bin:5:3610a686")])


    def test_a_reply_lost_to_a_restart_fails_its_step_and_the_run_goes_on(self):
        device = FakeDevice({"STATE": ["STATE:activity=Home"]})
        port = connect(device)

        def restart(p):  # the device restarts right after it took the press, before it answers
            device.after_command = None
            p.gone = True

        device.after_command = restart
        ok, out = run(port, ["cmd:BTN:RIGHT", "state"])
        self.assertFalse(ok)
        self.assertIn("NO REPLY to BTN:RIGHT (the port went away)", out)
        self.assertNotIn("LINK UNSYNCED", out)
        self.assertEqual(device.received, [(0, "BTN:RIGHT"), (1, "STATE")])
        self.assertNotIn("step failed: state", out)


class RepliesAreTheirCommands(unittest.TestCase):
    def test_a_line_of_another_command_is_not_taken(self):
        device = FakeDevice({"ACT:HOME": ["IDLE:123", "EVT:ACT Home", "ACT_OK"]})
        port = connect(device)
        ok, out = run(port, ["cmd:ACT:HOME"])
        self.assertTrue(ok, out)
        self.assertIn("ACT_OK", out)
        self.assertNotIn("IDLE:123", out)

    def test_no_reply_with_the_port_open_ends_the_run_even_with_keep_going(self):
        # The reply comes late, while the next command would wait: it must not pass for that one's.
        device = FakeDevice({"STATE": [(LATE, "STATE:activity=Old")]})
        port = connect(device)
        ok, out = run(port, ["state", "state", "check:activity=Old"])
        self.assertFalse(ok)
        self.assertIn("LINK UNSYNCED: no reply to CMD:STATE within 0.3 s", out)
        self.assertEqual(device.received, [(0, "STATE")], "the run stopped before the next command")
        self.assertNotIn("check activity", out)

    def test_an_error_reply_does_not_end_the_run(self):
        device = FakeDevice({"NOPE": ["CMD_ERROR:unknown command NOPE"], "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        ok, out = run(port, ["cmd:NOPE", "state"])
        self.assertFalse(ok)
        self.assertIn("step failed: cmd:NOPE", out)
        self.assertIn("STATE:activity=Home", out)
        self.assertNotIn("LINK UNSYNCED", out)

    def test_idle_busy_ends_the_run(self):
        # A wait from before the run still runs: its answer is still to come, and could pass for the next IDLE's.
        device = FakeDevice({"IDLE": ["IDLE_ERROR:busy"], "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        ok, out = run(port, ["idle", "state"])
        self.assertFalse(ok)
        self.assertIn("LINK UNSYNCED: IDLE_ERROR:busy", out)
        self.assertEqual(device.received, [(0, "IDLE")])

    def test_a_screenshot_sent_as_cmd_reads_its_frame_off_the_line(self):
        frame = bytes([0x0A, 0xFF])  # a newline byte among the pixels
        device = FakeDevice({"SCREENSHOT": ["SCREENSHOT_START:8:2:2", frame, "SCREENSHOT_END"],
                             "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        ok, out = run(port, ["cmd:SCREENSHOT", "state"])
        self.assertTrue(ok, out)
        self.assertIn("SCREENSHOT_END (not saved", out)
        self.assertIn("STATE:activity=Home", out)

    def test_an_unknown_command_is_answered_by_cmd_error(self):
        device = FakeDevice({"NOPE": ["CMD_ERROR:unknown command NOPE"]})
        port = connect(device)
        ok, out = run(port, ["cmd:NOPE"])
        self.assertFalse(ok)
        self.assertIn("CMD_ERROR:unknown command NOPE", out)

    def test_replies_under_another_word_are_still_their_commands(self):
        device = FakeDevice({
            "SWIPE:1,2,3,4": ["TOUCH_ERROR:no touch panel"],
            "ACT:DICT:word": ["DICT:result=found dict=x word=word"],
        })
        port = connect(device)
        ok, out = run(port, ["cmd:SWIPE:1,2,3,4", "cmd:ACT:DICT:word"])
        self.assertIn("TOUCH_ERROR:no touch panel", out)
        self.assertIn("DICT:result=found", out)
        self.assertNotIn("NO REPLY", out)

    def test_a_log_line_with_error_in_it_does_not_end_a_listing(self):
        device = FakeDevice({"GET:*": ["GET:a=1", "[123] [ERR] [SD] READ_ERROR on sector 7", "GET:b=2", "GET_END:2"]})
        port = connect(device)
        ok, out = run(port, ["cmd:GET:*"])
        self.assertTrue(ok, out)
        self.assertIn("GET:b=2", out)
        self.assertIn("GET_END:2", out)


class MalformedInputFailsCleanly(unittest.TestCase):
    def test_a_bad_number_or_pattern_fails_its_step_and_the_run_goes_on(self):
        device = FakeDevice({"STATE": ["STATE:activity=Home"]})
        port = connect(device)
        steps = ["wait:soon", "wait:inf", "expect:ready:soon", "expect:([:1", "check:activity=([", "select:([", "state"]
        ok, out = run(port, steps)
        self.assertFalse(ok)
        self.assertEqual(out.count("BAD STEP:"), 6, out)
        self.assertIn("STATE:activity=Home", out)
        self.assertEqual(device.received, [(0, "STATE")], "nothing was sent for the bad steps")

    def test_a_malformed_screenshot_start_stops_the_run(self):
        # The frame of unknown length that follows cannot be read off the line.
        device = FakeDevice({"SCREENSHOT": ["SCREENSHOT_START:800:x:48000"], "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        ok, out = run(port, ["cmd:SCREENSHOT", "state"])
        self.assertFalse(ok)
        self.assertIn("LINK UNSYNCED: malformed 'SCREENSHOT_START:800:x:48000'", out)
        self.assertEqual(device.received, [(0, "SCREENSHOT")])

    def test_a_malformed_upload_answer_stops_the_run(self):
        with tempfile.TemporaryDirectory() as folder:
            local = os.path.join(folder, "a.bin")
            with open(local, "wb") as f:
                f.write(b"hello")
            device = FakeDevice({"FS:PUT:/test/a.bin:5:3610a686": ["FS_PUT_READY:lots"]})
            port = connect(device)
            ok, out = run(port, [f"put:{local}:/test/a.bin", "state"])
        self.assertFalse(ok)
        self.assertIn("LINK UNSYNCED: malformed 'FS_PUT_READY:lots'", out)
        self.assertEqual(len(device.received), 1)

    def test_a_malformed_download_start_stops_the_run(self):
        # As the device does: the first raw piece follows the line at once, and an answer to a cancel would come
        # straight after its last byte, with no line break before it.
        device = FakeDevice({"FS:GET:/test/a.bin": ["FS_GET_START:zz", b"\x00\x01\x02piece"],
                             "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        started = time.time()
        with tempfile.TemporaryDirectory() as folder:
            ok, out = run(port, [f"get:/test/a.bin:{os.path.join(folder, 'a.bin')}", "state"])
        self.assertFalse(ok)
        self.assertIn("LINK UNSYNCED: malformed 'FS_GET_START:zz'", out)
        self.assertEqual(len(device.received), 1, "the run stopped before STATE")
        self.assertLess(time.time() - started, 5, "at once, not after a wait for an answer")

    def test_a_malformed_hash_answer_fails_its_step(self):
        device = FakeDevice({"FS:HASH:/test/a.bin": ["FS_HASH:nothex"], "STATE": ["STATE:activity=Home"]})
        port = connect(device)
        ok, out = run(port, ["hash:/test/a.bin=/dev/null", "state"])
        self.assertFalse(ok)
        self.assertIn("HASH FAILED: expected FS_HASH:<crc32hex> <size> <path>", out)
        self.assertIn("STATE:activity=Home", out)


class ClosingThePort(unittest.TestCase):
    def test_a_port_that_went_away_closes_without_an_error(self):
        # The device slept at the end of the run: the close must not turn a passing run into a traceback.
        port = connect(FakeDevice({}))
        port.ser.gone = True
        port.close()
        self.assertFalse(port.ser.is_open)

    def test_the_close_drops_rts_before_dtr(self):
        port = connect(FakeDevice({}))
        port.close()
        self.assertEqual(port.ser.lines, {"rts": False, "dtr": False})


class TheModelStaysInTheseTests(unittest.TestCase):
    def test_the_fake_serial_module_is_not_left_in_sys_modules(self):
        self.assertIsNot(sys.modules.get("serial"), devctl.serial)
        self.assertNotIn("devctl", sys.modules)


class WaitsAsLongAsTheDevice(unittest.TestCase):
    def test_the_reply_timeout_covers_what_the_command_takes_on_the_device(self):
        read = devctl.REPLY_TIMEOUT  # for the device to read the command
        cases = {
            "STATE": read,
            "IDLE": 10 + read,  # the device's default
            "IDLE:SCREEN": 10 + read,
            "IDLE:30000": 30 + read,
            " IDLE:30000 ": 30 + read,  # the device trims the text
            "IDLE:5000:SCREEN": 5 + read,
            "IDLE:+2000": 2 + read,
            "IDLE:9999999": 600 + read,  # the device's longest
            "SCREENSHOT": devctl.SCREENSHOT_WAIT + read,
            "FS:HASH:/test/a.epub": devctl.FILE_READ_TIMEOUT + read,
            "FS:GET:/test/a.epub": devctl.FILE_READ_TIMEOUT + read,
            "ACT:DICT:word": devctl.FILE_READ_TIMEOUT + read,
        }
        for text, seconds in cases.items():
            self.assertAlmostEqual(devctl.reply_timeout(text), seconds, msg=text)

    def test_an_idle_written_with_a_space_waits_for_its_own_timeout(self):
        # cmd: IDLE:1000 used to wait only the plain reply timeout, as its text did not start with IDLE.
        device = FakeDevice({" IDLE:1000": [(LATE, "IDLE:600")]})
        port = connect(device)
        ok, out = run(port, ["cmd: IDLE:1000"])
        self.assertTrue(ok, out)
        self.assertIn("IDLE:600", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
