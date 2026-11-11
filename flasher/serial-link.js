// Witch Reader web flasher: the firmware's own serial link, after a flash.
//
// Two protocols share the USB serial port (docs/serial-test-api.md):
//   - "CMD:" lines, answered with one line each (CMD:HELP, CMD:WEB, CMD:ACT:USB_TRANSFER, ...); the
//     device's log lines run between them, so a reply is picked out by its prefix;
//   - on the USB Transfer screen, the MicroReader-compatible file transfer
//     (lib/SerialTransfer/SerialTransfer/SerialTransferProtocol.h): "CMND" 'W' <u16 len> path
//     <u32 size>, "READY", 2048-byte chunks each acknowledged with 0x06, <u32 crc32>, "OK".
// installWebBundle() uses both to put web-<hash>.bin on the card and install it.

import { crc32 } from "./lib.js";

const ACK = 0x06;
const CHUNK = 2048;
const encoder = new TextEncoder();
const decoder = new TextDecoder();

const u16le = (v) => new Uint8Array([v & 0xff, (v >> 8) & 0xff]);
const u32le = (v) => new Uint8Array([v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]);

function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export class DeviceLink {
  constructor(port, log) {
    this.port = port;
    this.log = log || (() => {});
    this.buf = new Uint8Array(0);
    this.waiters = [];
    this.closed = false;
    this.reader = null;
    this.writer = null;
  }

  async open() {
    // The default DTR/RTS lines, as the port opens them: a change of these resets an ESP32-C3.
    await this.port.open({ baudRate: 115200, bufferSize: 16384 });
    this.writer = this.port.writable.getWriter();
    this.pump();
  }

  async pump() {
    try {
      while (!this.closed && this.port.readable) {
        this.reader = this.port.readable.getReader();
        try {
          for (;;) {
            const { value, done } = await this.reader.read();
            if (done) break;
            if (value && value.length) {
              this.buf = concat(this.buf, value);
              this.wake();
            }
          }
        } finally {
          this.reader.releaseLock();
          this.reader = null;
        }
      }
    } catch (e) {
      this.error = e;
    }
    this.closed = true;
    this.wake();
  }

  wake() {
    const waiters = this.waiters;
    this.waiters = [];
    waiters.forEach((f) => f());
  }

  waitForData(ms) {
    return new Promise((resolve) => {
      const timer = setTimeout(resolve, ms);
      this.waiters.push(() => {
        clearTimeout(timer);
        resolve();
      });
    });
  }

  async write(data) {
    await this.writer.write(typeof data === "string" ? encoder.encode(data) : data);
  }

  // The next whole line, or null at the deadline.
  async readLine(deadline) {
    for (;;) {
      const nl = this.buf.indexOf(10);
      if (nl >= 0) {
        const line = decoder.decode(this.buf.subarray(0, nl)).replace(/\r$/, "");
        this.buf = this.buf.slice(nl + 1);
        return line;
      }
      if (this.closed) throw new Error("the device went away");
      const left = deadline - Date.now();
      if (left <= 0) return null;
      await this.waitForData(left);
    }
  }

  // The first line starting with one of `prefixes`; log lines in between are skipped.
  async expectLine(prefixes, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const line = await this.readLine(deadline);
      if (line === null) return null;
      if (prefixes.some((p) => line.startsWith(p))) return line;
    }
  }

  async command(text, prefixes, timeoutMs = 5000) {
    await this.write(text + "\n");
    return this.expectLine(prefixes, timeoutMs);
  }

  // One acknowledgement byte of the transfer protocol. Anything else is a line ("ERR:...").
  async expectAck(timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      if (this.buf.length) {
        if (this.buf[0] === ACK) {
          this.buf = this.buf.slice(1);
          return;
        }
        const line = await this.readLine(deadline);
        if (line === null) throw new Error("no acknowledgement from the device");
        if (line.startsWith("ERR")) throw new Error("the device refused the file: " + line);
        continue;
      }
      if (this.closed) throw new Error("the device went away");
      const left = deadline - Date.now();
      if (left <= 0) throw new Error("no acknowledgement from the device");
      await this.waitForData(left);
    }
  }

  // The USB Transfer screen's 'W' message: `data` to `path` on the card, CRC-checked by the device.
  async writeFile(path, data, onProgress) {
    const name = encoder.encode(path);
    await this.write(concat(encoder.encode("CMNDW"), u16le(name.length), name, u32le(data.length)));
    const ready = await this.expectLine(["READY", "ERR"], 10000);
    if (ready !== "READY") throw new Error("the device did not accept the file: " + (ready || "no answer"));
    let crc = 0;
    for (let at = 0; at < data.length; at += CHUNK) {
      const chunk = data.subarray(at, at + CHUNK);
      await this.write(chunk);
      crc = crc32(chunk, crc);
      await this.expectAck(15000);
      if (onProgress) onProgress(at + chunk.length, data.length);
    }
    await this.write(u32le(crc));
    const done = await this.expectLine(["OK", "ERR"], 15000);
    if (done !== "OK") throw new Error("the device did not keep the file: " + (done || "no answer"));
  }

  // Lines drop RTS before DTR: on Windows a plain close drops DTR first, and an ESP32-C3 takes RTS
  // high with DTR low as a reset (docs/serial-test-api.md, "Connecting").
  async close() {
    this.closed = true;
    try {
      await this.port.setSignals({ requestToSend: false });
      await this.port.setSignals({ dataTerminalReady: false });
    } catch (e) {
      // the port is already gone
    }
    try {
      if (this.reader) await this.reader.cancel();
    } catch (e) {
      // nothing left to cancel
    }
    try {
      this.writer.releaseLock();
    } catch (e) {
      // no writer
    }
    try {
      await this.port.close();
    } catch (e) {
      // already closed
    }
  }
}

// "key=value key2=value2" of a WEB:/STATE: reply (values without spaces, or "quoted").
export function parseFields(text) {
  const out = {};
  const re = /(\w+)=("(?:[^"\\]|\\.)*"|\S*)/g;
  let m;
  while ((m = re.exec(text))) out[m[1]] = m[2].startsWith('"') ? m[2].slice(1, -1).replace(/\\(.)/g, "$1") : m[2];
  return out;
}

// After a flash: wait for the firmware to answer, then write the web bundle on the USB Transfer
// screen and install it with CMD:WEB:INSTALL. `bundle` is { name, data }, already checked against
// its name's hash. Returns a short result text; throws with the reason when it fails.
export async function installWebBundle(link, bundle, say) {
  // The C3 boards hold their output back until the host writes (docs/serial-test-api.md); a fresh
  // install's first start can take a while, so ask again until it answers.
  let help = null;
  const deadline = Date.now() + 40000;
  while (!help && Date.now() < deadline) {
    help = await link.command("\r\nCMD:HELP", ["HELP:"], 3000);
  }
  if (!help) throw new Error("the firmware did not answer over USB");
  if (!help.split(" ").includes("WEB")) {
    return "skipped: this firmware cannot install the web interface over USB (it has no CMD:WEB)";
  }
  const web = await link.command("CMD:WEB", ["WEB:", "WEB_ERROR"], 8000);
  if (!web || !web.startsWith("WEB:")) throw new Error("no answer to CMD:WEB");
  const state = parseFields(web.slice(4));
  say(`The firmware expects ${state.asset}; its web interface is ${state.state}.`);
  if (state.state === "installed") return "already installed";
  if (state.asset !== bundle.name) {
    return `skipped: the firmware expects ${state.asset}, this page has ${bundle.name}`;
  }
  const act = await link.command("CMD:ACT:USB_TRANSFER", ["ACT_OK", "ACT_ERROR", "CMD_ERROR"], 8000);
  if (act !== "ACT_OK") throw new Error("the USB Transfer screen did not open: " + (act || "no answer"));
  // The screen takes over the port's input once it is up; until then CMD:STATE names another.
  let onScreen = false;
  for (let i = 0; i < 30 && !onScreen; i++) {
    await sleep(500);
    const st = await link.command("CMD:STATE", ["STATE:"], 3000);
    onScreen = !!st && parseFields(st.slice(6)).activity === "SerialTransfer";
  }
  if (!onScreen) throw new Error("the USB Transfer screen did not come up");
  say(`Writing ${bundle.name} to the card...`);
  await link.writeFile("/" + bundle.name, bundle.data, (n, total) => say(`Writing ${bundle.name}: ${Math.floor((100 * n) / total)}%`, true));
  say("Installing the web interface on the device...");
  const result = await link.command("CMD:WEB:INSTALL", ["WEB_OK", "WEB_ERROR", "CMD_ERROR"], 60000);
  // Leaving the transfer screen restarts the device to Home, whatever the result.
  await link.write("CMD:ACT:HOME\n");
  if (result !== "WEB_OK") throw new Error("the device did not install it: " + (result || "no answer"));
  return "installed";
}
