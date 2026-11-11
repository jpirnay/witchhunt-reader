// Witch Reader web flasher: image and partition-table logic, with no browser API in it.
//
// The formats are ESP-IDF's: the image header (esp_image_header_t, the 0xE9 magic), the app
// description (esp_app_desc_t, magic 0xABCD5432) and the partition table (32-byte entries with the
// 0x50AA magic and an MD5 entry). scripts/full_install_image.py builds and checks the full images
// with the same rules; partitions.csv is the table "ours" means.

export const APP_OFFSET = 0x10000;
export const OTADATA_OFFSET = 0xe000;
export const OTADATA_SIZE = 0x2000;
export const TABLE_OFFSET = 0x8000;
export const TABLE_SIZE = 0xc00;

const IMAGE_MAGIC = 0xe9;
const APP_DESC_MAGIC = 0xabcd5432;
// esp_image_header_t.chip_id
export const CHIP_IDS = { "ESP32-C3": 5, "ESP32-S3": 9 };

// The boards the page offers. `chip` is esptool-js's CHIP_NAME; `reset` is how the device leaves
// the ROM loader after a write: the C3 boards take esptool's hard reset (RTS), the S3 boards a
// watchdog reset, as tools/devctl.py's flashing does.
export const BOARDS = [
  { id: "x3x4", label: "Xteink X3 / X4", chip: "ESP32-C3", reset: "hard" },
  { id: "x4pro", label: "Xteink X4 Pro", chip: "ESP32-S3", reset: "watchdog" },
  { id: "lilygo", label: "LilyGo T5 S3", chip: "ESP32-S3", reset: "watchdog" },
  { id: "murphy", label: "Murphy M3", chip: "ESP32-S3", reset: "watchdog" },
];

const u16 = (b, o) => b[o] | (b[o + 1] << 8);
const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

function cString(bytes) {
  const end = bytes.indexOf(0);
  return new TextDecoder("latin1").decode(end < 0 ? bytes : bytes.subarray(0, end));
}

// ---- Partition table ----------------------------------------------------------------------------

// Entries of the table in `blob` (read from 0x8000), or null when it is not a valid table: no
// entry, a bad entry, or an MD5 entry that does not match.
export function parseTable(blob) {
  const entries = [];
  const md5 = new Md5();
  for (let pos = 0; pos + 32 <= blob.length; pos += 32) {
    const e = blob.subarray(pos, pos + 32);
    if (e[0] === 0xaa && e[1] === 0x50) {
      entries.push({
        label: cString(e.subarray(12, 28)),
        type: e[2],
        subtype: e[3],
        offset: u32(e, 4),
        size: u32(e, 8),
        flags: u32(e, 28),
      });
      md5.update(e);
      continue;
    }
    if (e[0] === 0xeb && e[1] === 0xeb) {
      const want = toHex(e.subarray(16, 32));
      return entries.length && md5.hex() === want ? entries : null;
    }
    // Tables built without the MD5 entry end in 0xFF.
    return entries.length && e.every((x) => x === 0xff) ? entries : null;
  }
  return null;
}

const sameEntry = (a, b) =>
  a.label === b.label && a.type === b.type && a.subtype === b.subtype && a.offset === b.offset && a.size === b.size;

export const hasSpiffs = (entries) => entries.some((e) => e.type === 0x01 && e.subtype === 0x82);

// "ours" when the table is exactly `ours` (the release's), "stock" when it has an app at 0x10000 and
// no spiffs partition (a stock X4 Pro has none, which is issue #733), "unknown" otherwise.
export function classifyTable(entries, ours) {
  if (!entries) return { kind: "unknown", why: "no valid partition table at 0x8000 (empty or foreign flash)" };
  if (ours && entries.length === ours.length && entries.every((e, i) => sameEntry(e, ours[i]))) {
    return { kind: "ours", why: "Witch Reader's partition table" };
  }
  const app0 = entries.find((e) => e.type === 0x00 && e.offset === APP_OFFSET);
  if (app0 && !hasSpiffs(entries)) {
    return { kind: "stock", why: "a table without the spiffs area (stock firmware, or an old layout)" };
  }
  return { kind: "unknown", why: "a partition table that is not Witch Reader's" };
}

export function describeTable(entries) {
  const typeName = (t) => (t === 0 ? "app" : t === 1 ? "data" : "0x" + t.toString(16));
  return entries
    .map((e) => `${e.label.padEnd(9)} ${typeName(e.type)}/0x${e.subtype.toString(16).padStart(2, "0")} ` +
      `0x${e.offset.toString(16).padStart(6, "0")} +0x${e.size.toString(16)}`)
    .join("\n");
}

// ---- Image files --------------------------------------------------------------------------------

// What a .bin is, by its content:
//   full: an image header at 0 (the bootloader) and a partition table at 0x8000, so it is written
//         from 0x0;
//   app:  an app image (header, then the app description in its first segment), written at 0x10000.
// `chip` is the chip the image is built for, from the header's chip id.
export function detectImage(bytes) {
  if (bytes.length < 0x30 || bytes[0] !== IMAGE_MAGIC) return { kind: "unknown", why: "not an ESP32 image" };
  const chip = chipOf(u16(bytes, 12));
  if (bytes.length > TABLE_OFFSET + 2 && bytes[TABLE_OFFSET] === 0xaa && bytes[TABLE_OFFSET + 1] === 0x50) {
    const table = parseTable(bytes.subarray(TABLE_OFFSET, TABLE_OFFSET + TABLE_SIZE));
    if (!table) return { kind: "unknown", why: "a partition table at 0x8000 that does not parse" };
    const app = bytes.length > APP_OFFSET + 0x30 ? appInfo(bytes.subarray(APP_OFFSET)) : null;
    return { kind: "full", chip, table, app };
  }
  const app = appInfo(bytes);
  if (app) return { kind: "app", chip, app };
  return { kind: "unknown", why: "an image header, but neither an app nor a full image" };
}

function chipOf(id) {
  return Object.keys(CHIP_IDS).find((name) => CHIP_IDS[name] === id) || `chip id ${id}`;
}

// esp_app_desc_t sits at the start of the first segment: 24-byte image header + 8-byte segment
// header. Null when that is not an app.
export function appInfo(bytes) {
  if (bytes.length < 0x30 + 0x100 || bytes[0] !== IMAGE_MAGIC) return null;
  const desc = 24 + 8;
  if (u32(bytes, desc) !== APP_DESC_MAGIC) return null;
  return {
    version: cString(bytes.subarray(desc + 16, desc + 48)),
    project: cString(bytes.subarray(desc + 48, desc + 80)),
    time: cString(bytes.subarray(desc + 80, desc + 96)),
    date: cString(bytes.subarray(desc + 96, desc + 112)),
    chip: chipOf(u16(bytes, 12)),
  };
}

// ---- Checksums ----------------------------------------------------------------------------------

export function toHex(bytes) {
  return Array.from(bytes, (x) => x.toString(16).padStart(2, "0")).join("");
}

// CRC-32 (zlib/IEEE, poly 0xEDB88320), as the USB Transfer protocol checks an upload.
let crcTable = null;
export function crc32(bytes, crc = 0) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  crc = ~crc >>> 0;
  for (let i = 0; i < bytes.length; i++) crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  return ~crc >>> 0;
}

// MD5 (RFC 1321): esptool-js checks each written region against the flash's MD5, and WebCrypto has
// none. Incremental, for the partition table's MD5 entry as well.
const MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const MD5_K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);

export class Md5 {
  constructor() {
    this.h = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
    this.buf = new Uint8Array(64);
    this.bufLen = 0;
    this.total = 0;
    this.w = new Uint32Array(16);
  }

  update(bytes) {
    let i = 0;
    this.total += bytes.length;
    if (this.bufLen) {
      const take = Math.min(64 - this.bufLen, bytes.length);
      this.buf.set(bytes.subarray(0, take), this.bufLen);
      this.bufLen += take;
      i = take;
      if (this.bufLen < 64) return this;
      this.block(this.buf, 0);
      this.bufLen = 0;
    }
    for (; i + 64 <= bytes.length; i += 64) this.block(bytes, i);
    this.buf.set(bytes.subarray(i), 0);
    this.bufLen = bytes.length - i;
    return this;
  }

  block(b, o) {
    const w = this.w;
    for (let j = 0; j < 16; j++) w[j] = u32(b, o + 4 * j);
    let [a, bb, c, d] = this.h;
    for (let j = 0; j < 64; j++) {
      let f, g;
      if (j < 16) [f, g] = [(bb & c) | (~bb & d), j];
      else if (j < 32) [f, g] = [(d & bb) | (~d & c), (5 * j + 1) % 16];
      else if (j < 48) [f, g] = [bb ^ c ^ d, (3 * j + 5) % 16];
      else [f, g] = [c ^ (bb | ~d), (7 * j) % 16];
      const s = MD5_S[(j >> 4) * 4 + (j % 4)];
      const t = (a + f + MD5_K[j] + w[g]) >>> 0;
      a = d;
      d = c;
      c = bb;
      bb = (bb + ((t << s) | (t >>> (32 - s)))) >>> 0;
    }
    this.h = [(this.h[0] + a) >>> 0, (this.h[1] + bb) >>> 0, (this.h[2] + c) >>> 0, (this.h[3] + d) >>> 0];
  }

  hex() {
    const saved = { h: this.h.slice(), buf: this.buf.slice(), bufLen: this.bufLen, total: this.total };
    const bits = this.total * 8;
    const pad = new Uint8Array(((this.bufLen < 56 ? 56 : 120) - this.bufLen) + 8);
    pad[0] = 0x80;
    const view = new DataView(pad.buffer);
    view.setUint32(pad.length - 8, bits >>> 0, true);
    view.setUint32(pad.length - 4, Math.floor(bits / 2 ** 32), true);
    this.update(pad);
    const out = new Uint8Array(16);
    this.h.forEach((v, i) => new DataView(out.buffer).setUint32(4 * i, v, true));
    Object.assign(this, saved);
    return toHex(out);
  }
}

export const md5Hex = (bytes) => new Md5().update(bytes).hex();

// ---- The web bundle -----------------------------------------------------------------------------

// web-<hash>.bin is named after the first 12 hex digits of its SHA-256 (scripts/build_html.py), and
// the firmware checks the whole digest when it installs it. `sha256` is WebCrypto's digest.
export function bundleNameMatches(name, sha256Hex) {
  const m = /^web-([0-9a-f]{12})\.bin$/.exec(name);
  return !!m && sha256Hex.startsWith(m[1]);
}
