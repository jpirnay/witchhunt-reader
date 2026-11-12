#!/usr/bin/env python3
"""Drive the reader over USB serial with the serial test API (docs/serial-test-api.md).

    tools/devctl.py [--port P] [--log FILE] [--rotate DEG] STEP...
    tools/devctl.py [--port P] --script steps.txt

Steps run in order; the run stops at the first failed step and exits non-zero (--keep-going runs
them all and exits non-zero if any failed).

  cmd:<TEXT>            send "CMD:<TEXT>", wait for its reply line, print it
                        (fails on a *_ERROR / *_TIMEOUT reply or none at all)
  state                 CMD:STATE, print the reply and remember its fields for `check:`
  check:<key>=<regex>   the last STATE's <key> must match <regex> (re.fullmatch)
  idle[:<ms>[:SCREEN]]  CMD:IDLE..., wait for IDLE / IDLE_TIMEOUT (a timeout fails the step)
  shot:<file.png>       CMD:SCREENSHOT, decode the 1-bit frame, rotate it, save it
  wait:<seconds>        read (and log) the device's output for a while
  expect:<regex>:<sec>  read until an output line matches (re.search), fail otherwise
  select:<regex>[:<n>]  press Down, waiting for each, until STATE's label matches (re.fullmatch); at most n
                        presses (default 40). The label is checked first, so a row already selected takes none.
  page:<out.txt>        CMD:PAGE, write the reader's page dump (its PAGE_ lines) to the file, print a summary;
                        fails when the dump is not whole (a line or a word missing, out of order or garbled)
  put:<local>:<remote>  upload a file (CMD:FS:PUT), checked by its CRC32
  get:<remote>:<local>[:<n>]  download a file (CMD:FS:GET), acknowledging each piece, checked by its size and
                        CRC32; after a failure, tried again up to n times (default 0)
  hash:<remote>[=<local>]  CMD:FS:HASH; with a local file, fail unless both size and CRC32 match it
  mkdir:<remote>        CMD:FS:MKDIR (with the folders above it)
  mv:<from>:<to>        CMD:FS:MV (rename or move on the card)

Card paths (<remote>) start with '/' and hold no ':' (FAT names cannot), so a local path may carry a
drive letter: put:C:/books/a.epub:/test/a.epub splits at its last ':', get:/test/a.epub:C:/tmp/a.epub
at its first. hash: splits at the first '=', so a card path with '=' in it cannot be compared there
(use cmd:FS:HASH:<path>). Local paths may be given as Git Bash writes them (/c/tmp/a.epub).

A step takes only a reply to its own command: a line that starts with the command's name and '_' or ':'
(BTN_OK, STATE:..., IDLE_TIMEOUT:...), or CMD_ERROR. Log lines, and a late reply to an earlier command,
are skipped. After a step got no reply, the next command first reads until the line has been quiet for
1 s (at most 10 s), so that reply, should it still come, cannot pass for the next one's.

A --script file holds one step per line; blank lines and lines starting with # are skipped.
Every line the device prints goes to --log with a timestamp. Needs pyserial, and Pillow for shot:.

The port is opened with the default DTR/RTS lines, which does not reset an ESP32-C3 (unlike
tools/serial_transfer.py, which deasserts them), and closed with RTS dropped before DTR, which does
not either (a plain close on Windows does). The device re-enumerates its USB port across a
reboot or a sleep; the reader reopens it and carries on. A command (or a piece of a put:, or a get:'s
acknowledgement) whose write fails that way is not sent again, since the device may have taken it:
its step fails with DELIVERY UNCERTAIN, and the next step uses the reopened port.
"""
import argparse
import os
import re
import sys
import time
import zlib

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

# A reply to CMD:<NAME>... starts with NAME and '_' or ':' (BTN_OK, STATE:..., IDLE_TIMEOUT:..., GET_END:...; see
# docs/serial-test-api.md), or is CMD_ERROR (an unknown command). These commands also answer under another word: SWIPE's
# errors are TOUCH_ERROR (no panel, not on screen), and ACT:DICT answers DICT:result=... or DICT_ERROR.
OTHER_REPLIES = {"SWIPE": ("TOUCH_",), "ACT": ("DICT:", "DICT_")}
# Seconds a step waits for its reply when it names no other wait.
REPLY_TIMEOUT = 20
# After a command got no reply, the next one first reads the line until it has been quiet this long (at most
# RESYNC_MAX seconds): that reply, should it still come, is dropped there rather than taken for the next one's.
RESYNC_QUIET = 1.0
RESYNC_MAX = 10.0
# Commands that answer with several lines, and the line that ends them.
MULTI_LINE = {"GET:*": re.compile(r"^GET_END"), "FS:LS:": re.compile(r"^FS_END"), "PAGE": re.compile(r"^PAGE_END")}
# A file the device reads through for its CRC32 before it answers (FS:GET, FS:HASH): seconds a megabyte, and then
# some.
FILE_READ_TIMEOUT = 300
# FS:GET sends a piece only once the host has acknowledged the one before (the transfer protocol's ACK byte); any
# other byte cancels. A piece the device has ready arrives in milliseconds, so one still short after this many
# seconds lost bytes on the way.
GET_ACK = b"\x06"
GET_CANCEL = b"\x18"
GET_PIECE_IDLE = 5
FAILED = re.compile(r"_ERROR|_TIMEOUT|^CMD_ERROR")
# Seconds to give a port that went away (a reboot, a sleep) before it is opened again.
REOPEN_PAUSE = 1.0


class DeliveryUncertain(Exception):
    """A write failed because the port went away. The port is open again, but whether the device took the bytes is
    unknown, so what was being written is not sent again: a press or a file write must not happen twice."""


def reply_prefixes(text):
    """What a reply to CMD:<text> starts with (see OTHER_REPLIES)."""
    name = text.split(":", 1)[0].strip()
    return (f"{name}_", f"{name}:", "CMD_ERROR") + OTHER_REPLIES.get(name, ())


def native_path(path):
    """A Git Bash path (/c/tmp/x.png) as Windows' Python opens it (C:/tmp/x.png); others as they are."""
    match = re.match(r"^/([a-zA-Z])/(.*)$", path)
    return f"{match.group(1).upper()}:/{match.group(2)}" if os.name == "nt" and match else path


def split_fields(text):
    """key=value pairs of a STATE line; values in double quotes may hold spaces (\\ escapes)."""
    fields = {}
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i] == " ":
            i += 1
        if i >= n:
            break
        eq = text.find("=", i)
        if eq < 0:
            break
        key = text[i:eq]
        i = eq + 1
        value = []
        if i < n and text[i] == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    i += 1
                value.append(text[i])
                i += 1
            i += 1
        else:
            while i < n and text[i] != " ":
                value.append(text[i])
                i += 1
        fields[key] = "".join(value)
    return fields


PAGE_LINE_FORMS = {
    "PAGE_LINE": re.compile(r"(\d+) x=-?\d+ y=-?\d+ words=(\d+) align=\w+ vertical=[01]( .*)?"),
    "PAGE_W": re.compile(r'(\d+) x=-?\d+ w=-?\d+ style=\d+ glue=[01] text="(?:[^"\\]|\\.)*"( .*)?'),
    "PAGE_IMAGE": re.compile(r"(\d+) x=-?\d+ y=-?\d+ w=-?\d+ h=-?\d+"),
    "PAGE_EL": re.compile(r"(\d+) \w+"),
    "PAGE_BOX": re.compile(r"(\d+) x=-?\d+ y=-?\d+ w=\d+ h=\d+ edges=\d+/\d+(?:,\d+/\d+){3} shaded=[01]"),
}


def check_page(lines):
    """None when a CMD:PAGE dump is whole, else what is wrong with it. The dump has no checksum, but its shape gives a
    lost packet away: every element numbered in turn, a line's words all there and numbered, PAGE_END's count."""
    start = re.fullmatch(r"PAGE_START:spine=\S+ page=\S+ flow=\S+ elements=(\d+)", lines[0])
    if not start:
        return f"bad first line {lines[0]!r}"
    elements = int(start.group(1))
    element = 0
    words_left = 0
    word = 0
    for line in lines[1:-1]:
        kind, _, rest = line.partition(":")
        form = PAGE_LINE_FORMS.get(kind)
        match = form.fullmatch(rest) if form else None
        if not match:
            return f"garbled line {line[:60]!r}"
        if kind == "PAGE_W":
            if words_left == 0 or int(match.group(1)) != word:
                return f"word out of place in element {element - 1}: {line[:60]!r}"
            word += 1
            words_left -= 1
            continue
        if words_left:
            return f"element {element - 1} lacks {words_left} words"
        if int(match.group(1)) != element:
            return f"element {match.group(1)} where {element} was due"
        element += 1
        if kind == "PAGE_LINE":
            words_left = int(match.group(2))
            word = 0
    if words_left:
        return f"element {element - 1} lacks {words_left} words"
    end = re.fullmatch(r"PAGE_END:(\d+)( .*)?", lines[-1])
    if not end or int(end.group(1)) != elements or element != elements:
        return f"{element} elements of {elements}, ended by {lines[-1]!r}"
    return None


class Device:
    def __init__(self, port, log_path=None):
        self.port = port
        self.log = open(log_path, "a", encoding="utf-8", errors="replace") if log_path else None
        self.t0 = time.time()
        self.buf = b""
        self.read_pause = 0.0  # --read-pause: seconds get: waits after every MB, as a slow host would
        self.expected = ()  # what a reply to the command sent last starts with (reply_prefixes)
        self.unanswered = False  # it got no reply: the next command resyncs first
        self.ser = self._open()

    def note(self, line):
        if self.log:
            self.log.write(f"[{time.time() - self.t0:8.2f}] {line}\n")
            self.log.flush()

    def _open(self):
        for _ in range(60):
            try:
                s = serial.Serial(self.port, 115200, timeout=0.2)
                s.write(b"\r\n")  # C3 USB-CDC output waits until the host has written something
                return s
            except (serial.SerialException, OSError):
                time.sleep(0.5)
        sys.exit(f"cannot open {self.port}")

    def close(self):
        # RTS goes low before DTR: a plain close on Windows drops DTR first, and the ESP32-C3's USB serial unit
        # takes RTS high with DTR low as a reset request (esptool's), so every run restarted the device to Home.
        if self.ser.is_open:
            self.ser.rts = False
            self.ser.dtr = False
        self.ser.close()

    def _reopen(self):
        """The port went away (reboot, sleep, re-enumeration): open it again."""
        try:
            self.close()
        except Exception:
            pass
        time.sleep(REOPEN_PAUSE)
        self.ser = self._open()

    def _read(self, size):
        try:
            return self.ser.read(size)
        except (serial.SerialException, OSError):
            self._reopen()
            return b""

    def _write(self, data, what):
        """Write `data`. When the port has gone away, reopen it and raise DeliveryUncertain naming `what`: the step
        fails, and the next one goes to the reopened port."""
        try:
            self.ser.write(data)
        except (serial.SerialException, OSError):
            self._reopen()
            raise DeliveryUncertain(what) from None

    def _read_some(self):
        """What has arrived, or the next byte within the port's timeout. A read of a fixed size waits out the whole
        timeout for bytes that are not coming, which an upload paid on every chunk's acknowledgement."""
        try:
            waiting = self.ser.in_waiting
        except (serial.SerialException, OSError):
            waiting = 0
        return self._read(min(max(waiting, 1), 65536))

    def read_line(self, deadline):
        while time.time() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl], self.buf[nl + 1:]
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                self.note(text)
                return text
            self.buf += self._read_some()
        return None

    def read_exact(self, size, deadline):
        while len(self.buf) < size and time.time() < deadline:
            self.buf += self._read(min(65536, size - len(self.buf)))
        data, self.buf = self.buf[:size], self.buf[size:]
        return data

    def read_bulk(self, size, idle):
        """`size` raw bytes; gives up when none come for `idle` seconds. Gathered in a bytearray: a payload of
        megabytes added to a bytes object piece by piece is copied over and over."""
        out = bytearray(self.buf[:size])
        self.buf = self.buf[size:]
        deadline = time.time() + idle
        while len(out) < size and time.time() < deadline:
            piece = self._read(min(65536, size - len(out)))
            if piece:
                out += piece
                deadline = time.time() + idle
        return bytes(out)

    def is_reply(self, line):
        """Whether `line` answers the command sent last: not a log line, nor a late reply to an earlier command."""
        return line.startswith(self.expected)

    def reply(self, timeout):
        """The next reply line to the command sent last; other lines are skipped. None after `timeout` seconds."""
        deadline = time.time() + timeout
        while (line := self.read_line(deadline)) is not None:
            if self.is_reply(line):
                return line
        self.unanswered = True
        return None

    def resync(self):
        """After a command that got no reply: read until the line has been quiet for RESYNC_QUIET seconds (at most
        RESYNC_MAX), so that reply, should it come late, is dropped by drain() rather than taken for the next one's.
        A late reply of another command's kind would be skipped anyway (is_reply); this catches one of the same kind."""
        if not self.unanswered:
            return
        self.unanswered = False
        end = time.time() + RESYNC_MAX
        quiet_until = time.time() + RESYNC_QUIET
        while time.time() < min(end, quiet_until):
            got = self._read_some()
            if got:
                self.buf += got
                quiet_until = time.time() + RESYNC_QUIET

    def drain(self):
        """Log and drop the lines already received, so a late reply to an earlier command (one that
        timed out here) cannot pass for the reply to the next. A partial line is kept. A port that went
        away since the last step is opened again here, so the next command goes to the live one."""
        try:
            waiting = self.ser.in_waiting
        except (serial.SerialException, OSError):
            self._reopen()
            waiting = 0
        if waiting:
            self.buf += self._read(waiting)
        while (nl := self.buf.find(b"\n")) >= 0:
            line, self.buf = self.buf[:nl], self.buf[nl + 1:]
            self.note(line.decode("utf-8", errors="replace").rstrip("\r"))

    def send(self, text):
        self.resync()
        self.drain()
        self.note(f">>> CMD:{text}")
        self.expected = reply_prefixes(text)
        self._write(("CMD:" + text + "\n").encode(), f"CMD:{text}")

    def command(self, text, timeout=None, until=None, lines=None):
        """Send a command; return its reply line (the terminator of a several-line reply), or None after `timeout`
        seconds (REPLY_TIMEOUT when not given). With `until`, the reply lines before the terminator go to `lines`."""
        self.send(text)
        deadline = time.time() + (REPLY_TIMEOUT if timeout is None else timeout)
        while (line := self.read_line(deadline)) is not None:
            if not self.is_reply(line):
                continue  # a log line, or a late reply to an earlier command
            if until is None or until.match(line) or FAILED.search(line):
                return line
            if lines is not None:
                lines.append(line)
        self.unanswered = True
        return None

    def screenshot(self, path, rotate):
        self.send("SCREENSHOT")
        # Until it is answered in full, the frame's bytes may still be on their way: the next command resyncs first.
        self.unanswered = True
        deadline = time.time() + 30
        while (line := self.read_line(deadline)) is not None:
            if line.startswith("SCREENSHOT_START:"):
                _, w, h, size = line.split(":")
                data = self.read_exact(int(size), time.time() + 30)
                if len(data) != int(size):
                    return f"short frame: {len(data)} of {size} bytes"
                from PIL import Image  # only shot: needs Pillow

                image = Image.frombytes("1", (int(w), int(h)), data)
                if rotate:
                    image = image.rotate(rotate, expand=True)
                image.save(path)
                end = self.read_line(time.time() + 5)
                if end != "SCREENSHOT_END":
                    return f"no SCREENSHOT_END (got {end!r})"
                self.unanswered = False
                return None
            if line.startswith("SCREENSHOT_ERROR"):
                self.unanswered = False
                return line
        return "no reply"

    def page(self, path):
        """CMD:PAGE into `path`, one PAGE_ line a line. Returns None, or what went wrong."""
        self.send("PAGE")
        lines = []
        deadline = time.time() + 30
        while (line := self.read_line(deadline)) is not None:
            if not line.startswith("PAGE_"):
                continue  # a log line
            lines.append(line)
            if line.startswith(("PAGE_END", "PAGE_ERROR")):
                break
        if not lines:
            self.unanswered = True
            return "no reply"
        if lines[-1].startswith("PAGE_ERROR"):
            return lines[-1]
        if not lines[-1].startswith("PAGE_END"):
            self.unanswered = True  # the rest of the dump may still come
            return f"no PAGE_END after {len(lines)} lines (the last {lines[-1][:60]!r})"
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        damage = check_page(lines)
        if damage:
            return f"the dump is not whole ({damage}; bytes lost on the link?), written to {path} as it came"
        start = split_fields(lines[0][len("PAGE_START:"):])
        text_lines = sum(line.startswith("PAGE_LINE:") for line in lines)
        words = sum(line.startswith("PAGE_W:") for line in lines)
        images = sum(line.startswith("PAGE_IMAGE:") for line in lines)
        boxes = sum(line.startswith("PAGE_BOX:") for line in lines)
        print(f"page spine={start.get('spine')} page={start.get('page')} flow={start.get('flow')}: "
              f"{start.get('elements')} elements ({text_lines} lines, {images} images, {boxes} boxes), {words} words "
              f"-> {path}")
        return None

    def put(self, local, remote):
        """CMD:FS:PUT: the file in the chunks the device asks for, each acknowledged before the next."""
        with open(local, "rb") as f:
            data = f.read()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.send(f"FS:PUT:{remote}:{len(data)}:{crc:08x}")
        started = time.time()
        line = self.reply(20)
        if not line or not line.startswith("FS_PUT_READY:"):
            return line or "no reply"
        chunk = int(line.split(":", 1)[1])
        sent = 0
        while sent < len(data):
            piece = data[sent:sent + chunk]
            self._write(piece, f"FS:PUT:{remote}, the piece at byte {sent}")
            sent += len(piece)
            ack = self.reply(15)
            if ack != f"FS_PUT_ACK:{sent}":
                return ack or f"no FS_PUT_ACK:{sent}"
        done = self.reply(20)
        if not done or not done.startswith("FS_OK:put"):
            return done or "no FS_OK"
        seconds = time.time() - started
        print(f"{done} ({len(data) / 1024 / max(seconds, 0.001):.0f} KB/s)")
        return None

    def get(self, remote, local, retries=0):
        """CMD:FS:GET into `local`, written only when its size and CRC32 match what the device announced; after a
        failure, tried again up to `retries` times."""
        for attempt in range(retries + 1):
            error = self._get_once(remote, local)
            # Only a damaged transfer is worth repeating; the device's own refusals (not found, protected, no
            # memory, ...) come back the same every time.
            if error is None or attempt == retries or error.startswith("FS_ERROR:") and "stalled" not in error:
                return error
            print(f"GET {remote}: {error}; trying again ({attempt + 1} of {retries})")
        return None

    def _get_once(self, remote, local):
        self.send(f"FS:GET:{remote}")
        line = self.reply(FILE_READ_TIMEOUT)
        if not line or not line.startswith("FS_GET_START:"):
            return line or "no reply"
        fields = line.split(":")
        if len(fields) != 4:
            return f"expected FS_GET_START:<size>:<crc32hex>:<piece>, got {line!r}"
        try:
            size, crc, piece = int(fields[1]), int(fields[2], 16), int(fields[3])
        except ValueError:
            return f"bad FS_GET_START {line!r}"
        started = time.time()
        data = bytearray()
        next_pause = 1 << 20
        while len(data) < size:
            want = min(piece, size - len(data))
            got = self.read_bulk(want, GET_PIECE_IDLE)
            data += got
            if len(got) < want:
                return self._abandon_get(f"short piece at byte {len(data) - len(got)}: {len(got)} of {want} bytes "
                                         f"({len(data)} of {size} received)", got)
            # The device sends the next piece only now.
            self._write(GET_ACK, f"FS:GET:{remote}, the acknowledgement of byte {len(data)}")
            if self.read_pause and len(data) >= next_pause:
                time.sleep(self.read_pause)  # a slow host, for testing: the next piece waits in the host's buffer
                next_pause += 1 << 20
        end = self.reply(20)
        mine = zlib.crc32(data) & 0xFFFFFFFF
        if end != "FS_GET_END":
            return f"{end or 'no FS_GET_END'} (received {len(data)} of {size} bytes, crc {mine:08x})"
        if mine != crc:
            return f"crc mismatch: received {len(data)} of {size} bytes, crc {mine:08x}, the device sent {crc:08x}"
        with open(local, "wb") as f:
            f.write(data)
        seconds = time.time() - started
        print(f"{remote} -> {local}: {size} bytes, crc {crc:08x} ({size / 1024 / max(seconds, 0.001):.0f} KB/s)")
        return None

    def _abandon_get(self, reason, piece):
        """A piece came up short. If the device stopped with an error, it is in the piece; otherwise it waits for an
        acknowledgement that will not come: cancel, and read up to its answer, so the next command starts clean."""
        at = piece.find(b"FS_ERROR:")
        if at >= 0:
            told = piece[at:].split(b"\n", 1)[0].decode("utf-8", errors="replace").rstrip("\r")
            self.note(told)
            return f"{reason}; the device said {told}"
        self._write(GET_CANCEL, "the FS:GET cancel")
        answer = self.reply(GET_PIECE_IDLE)
        return f"{reason}; cancelled ({answer or 'no answer'})"

    def hash(self, remote, local=None):
        """CMD:FS:HASH, compared with `local` when given."""
        self.send(f"FS:HASH:{remote}")
        line = self.reply(FILE_READ_TIMEOUT)
        print(line if line else f"NO REPLY to FS:HASH:{remote}")
        if not line or not line.startswith("FS_HASH:"):
            return line or "no reply"
        crc, size, _ = line[len("FS_HASH:"):].split(" ", 2)
        if local is None:
            return None
        with open(local, "rb") as f:
            data = f.read()
        mine = f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"
        if (crc, int(size)) != (mine, len(data)):
            return f"differs from {local}: crc {mine}, {len(data)} bytes"
        print(f"matches {local}")
        return None


def idle_timeout(arg):
    """Seconds to wait for the reply to CMD:IDLE<arg>: the device's own timeout and some slack."""
    first = arg.split(":")[0] if arg else ""
    return (int(first) if first.isdigit() else 10000) / 1000 + 5


def file_step(device, kind, rest):
    """page:, put:, get:, hash:, mkdir:, mv:. Returns None, or what went wrong. A card path starts with '/' and holds
    no ':', so the step splits where a local path's drive letter cannot confuse it (see the module doc)."""
    if kind == "page":
        return device.page(native_path(rest)) if rest else "expected page:<out.txt>"
    if kind == "put":
        local, _, remote = rest.rpartition(":")
        if not local or not remote.startswith("/"):
            return "expected put:<local>:/<remote>"
        return device.put(native_path(local), remote)
    if kind == "get":
        remote, _, local = rest.partition(":")
        # A trailing ":<n>" is the retry count; a Windows file name cannot hold ':', so it cannot be part of the path.
        retries = 0
        head, _, tail = local.rpartition(":")
        if head and tail.isdigit():
            local, retries = head, int(tail)
        if not remote.startswith("/") or not local:
            return "expected get:/<remote>:<local>[:<retries>]"
        return device.get(remote, native_path(local), retries)
    if kind == "hash":
        remote, _, local = rest.partition("=")
        if not remote.startswith("/"):
            return "expected hash:/<remote>[=<local>]"
        return device.hash(remote, native_path(local) if local else None)
    if kind == "mkdir":
        command = f"FS:MKDIR:{rest}"
    else:
        source, _, target = rest.partition(":")
        if not source.startswith("/") or not target.startswith("/"):
            return "expected mv:/<from>:/<to>"
        command = f"FS:MV:{source}:{target}"
    reply = device.command(command)
    print(reply if reply else f"NO REPLY to {command}")
    return None if reply and reply.startswith("FS_OK:") else (reply or "no reply")


def run(device, steps, rotate, keep_going=False):
    state = {}
    all_ok = True
    for step in steps:
        try:
            ok, state = run_step(device, step, rotate, state)
        except DeliveryUncertain as error:
            # Not sent again: the device may have taken it before the port went away, and a press or a file write
            # must not happen twice.
            print(f"DELIVERY UNCERTAIN: {error} (port reopened, not resent)")
            ok = False
        if not ok:
            print(f"step failed: {step}")
            all_ok = False
            if not keep_going:
                return False
    return all_ok


def run_step(device, step, rotate, state):
    """One step. Returns whether it passed, and the STATE fields `check:` reads from then on."""
    kind, _, rest = step.partition(":")
    ok = True
    if kind == "cmd":
        until = next((end for start, end in MULTI_LINE.items() if rest.startswith(start)), None)
        timeout = idle_timeout(rest[5:]) if rest.startswith("IDLE") else REPLY_TIMEOUT
        lines = []
        reply = device.command(rest, timeout=timeout, until=until, lines=lines)
        for line in lines:
            print(line)
        print(reply if reply else f"NO REPLY to {rest}")
        ok = bool(reply) and not FAILED.search(reply)
    elif kind == "state":
        reply = device.command("STATE")
        print(reply if reply else "NO REPLY to STATE")
        ok = bool(reply) and reply.startswith("STATE:")
        if ok:
            state = split_fields(reply[len("STATE:"):])
    elif kind == "check":
        key, _, pattern = rest.partition("=")
        value = state.get(key)
        ok = value is not None and re.fullmatch(pattern, value) is not None
        print(f"check {key}={value!r} ~ /{pattern}/: {'ok' if ok else 'FAILED'}")
    elif kind == "idle":
        reply = device.command("IDLE" + (":" + rest if rest else ""), timeout=idle_timeout(rest))
        print(reply if reply else "NO REPLY to IDLE")
        ok = bool(reply) and reply.startswith("IDLE:")
    elif kind == "shot":
        path = native_path(rest)
        error = device.screenshot(path, rotate)
        print(f"screenshot -> {path}" if error is None else f"SCREENSHOT FAILED: {error}")
        ok = error is None
    elif kind == "select":
        pattern, _, most = rest.partition(":")
        presses = int(most) if most.isdigit() else 40
        ok = False
        for press in range(presses + 1):
            reply = device.command("STATE")
            fields = split_fields(reply[len("STATE:"):]) if reply and reply.startswith("STATE:") else {}
            if re.fullmatch(pattern, fields.get("label", "")):
                state = fields
                ok = True
                print(f"selected {fields.get('label')!r} after {press} presses")
                break
            if press == presses:
                break
            # One press at a time: presses sent back to back reach the list faster than it redraws, and it
            # moves further than asked.
            device.command("BTN:DOWN")
            device.command("IDLE:5000", timeout=idle_timeout("5000"))
        if not ok:
            print(f"NOT SELECTED: {pattern} (last label {fields.get('label')!r})")
    elif kind in ("page", "put", "get", "hash", "mkdir", "mv"):
        error = file_step(device, kind, rest)
        if error:
            print(f"{kind.upper()} FAILED: {error}")
        ok = error is None
    elif kind == "wait":
        end = time.time() + float(rest)
        while device.read_line(end) is not None:
            pass
    elif kind == "expect":
        pattern, _, seconds = rest.rpartition(":")
        end = time.time() + float(seconds)
        found = None
        while (line := device.read_line(end)) is not None:
            if re.search(pattern, line):
                found = line
                break
        print(f"matched: {found}" if found else f"NOT SEEN: {pattern}")
        ok = found is not None
    else:
        print(f"unknown step {step!r}")
        ok = False
    return ok, state


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--log", default=None, help="append the device's output here, timestamped")
    ap.add_argument("--rotate", type=int, default=-90,
                    help="rotate screenshots by this many degrees (default -90: the X4's 800x480 frame "
                         "upright in portrait; 0 keeps the panel's native frame)")
    ap.add_argument("--script", default=None, help="read steps from this file, one per line")
    ap.add_argument("--keep-going", action="store_true", help="run every step even after one failed")
    ap.add_argument("--read-pause", type=float, default=0.0,
                    help="get: stops reading this many seconds after every MB, as a slow host would (a test of the "
                         "device's pacing)")
    ap.add_argument("steps", nargs="*")
    args = ap.parse_args()

    steps = list(args.steps)
    if args.script:
        with open(args.script, encoding="utf-8") as f:
            steps += [s.strip() for s in f if s.strip() and not s.lstrip().startswith("#")]
    if not steps:
        ap.error("no steps given")

    # Labels, chapter titles and popups may be in any script: a console's code page must not stop the run.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    device = Device(args.port, native_path(args.log) if args.log else None)
    device.read_pause = args.read_pause
    try:
        ok = run(device, steps, args.rotate, args.keep_going)
    finally:
        # Whatever went wrong, the port closes RTS before DTR: an exception that closed it plainly would reset the
        # device (see close()).
        device.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
