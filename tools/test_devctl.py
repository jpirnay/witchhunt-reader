#!/usr/bin/env python3
"""Host tests of tools/devctl.py against a model of the device's port: no pyserial and no device needed. They cover
what a run does when the port goes away between or during steps. Run by ctest (test/devctl)."""
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
    order: a line at once, or (seconds, line) for one that comes that much later. Every opening of the port is a
    FakePort; output goes to the newest one, as the device's USB port is one."""

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
            self.opened[-1].incoming += (self.later.pop(0)[1] + "\n").encode()

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
        self.rts = True
        self.dtr = True
        self.incoming = bytearray()
        self.partial = b""
        self.gone = False  # the device rebooted: every call fails, as on a port that went away
        self.fail_write = False  # it goes away during the next write

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

    def close(self):
        self.is_open = False


class Harness:
    device = None


fake_serial = types.ModuleType("serial")
fake_serial.SerialException = PortGone
fake_serial.Serial = lambda *args, **kwargs: Harness.device.open()
sys.modules["serial"] = fake_serial
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import devctl  # noqa: E402  (after the fake serial module is in place)

devctl.REOPEN_PAUSE = 0
# Short waits, in the same proportions: a late reply comes 0.3 s after the host gave up on it, which leaves the
# resync 0.3 s to spare at either end.
devctl.REPLY_TIMEOUT = 0.3
devctl.RESYNC_QUIET = 0.6
devctl.RESYNC_MAX = 3
LATE = 0.6


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
        device.opened[0].gone = True  # the device restarted while the host was between steps
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


class RepliesMatchTheirCommand(unittest.TestCase):
    def test_a_late_reply_of_another_command_is_not_taken(self):
        # STATE gets no reply in time; its reply comes while ACT:HOME waits, and ACT:HOME gets none.
        device = FakeDevice({"ACT:HOME": ["STATE:activity=Home"]})
        port = connect(device)
        ok, out = run(port, ["cmd:STATE", "cmd:ACT:HOME"])
        self.assertFalse(ok)
        self.assertIn("NO REPLY to ACT:HOME", out)
        self.assertIn("step failed: cmd:ACT:HOME", out)

    def test_a_late_reply_of_the_same_kind_is_dropped_before_the_next_command(self):
        device = FakeDevice({"STATE": [(LATE, "STATE:activity=Old")]})
        port = connect(device)
        ok, out = run(port, ["state"])
        self.assertFalse(ok, out)  # timed out; the reply is still on its way
        device.answers["STATE"] = ["STATE:activity=New"]
        ok, out = run(port, ["state", "check:activity=New"])
        self.assertTrue(ok, out)
        self.assertNotIn("activity=Old", out)

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
