#!/usr/bin/env node
// Tests the web flasher's partition-table logic (flasher/lib.js): what it makes of the table it reads
// from a device, and the note it shows for it.
//
//   * AREA_BYTES is the `spiffs` size of partitions.csv and FlashArea::kExpectedBytes, the line below
//     which an X3 or X4 runs in emergency mode;
//   * tables are written as the chip holds them (32-byte entries and the MD5 entry) and read back with
//     parseTable, then classified against our table;
//   * an X3/X4 table with a small or no `spiffs` partition is named emergency mode with the device
//     notice's sizes; the same table on an S3 board is not, since those boards use PSRAM instead;
//   * Onyx's Picco table (partitions_picco.csv) is recognised, and its note sets out the two choices:
//     the full install (Onyx's boot loader and recovery replaced, self-updates) or Update only into
//     Onyx's slot (table kept, no self-update);
//   * a firmware image alone goes into the device's own ota_0 slot with its otadata: 0x10000 and
//     0xE000 on ours, 0xE0000 and 0x15000 on Onyx's, nowhere on any other table; image first, and on
//     Onyx's table with otadata byte for byte Arduino-ESP32's boot_app0.bin;
//   * the backup reads the whole flash in pieces, each checked against the chip's MD5: a bad or failed
//     piece is read again, one that stays bad fails the backup instead of reaching the file;
//   * of the releases the site offers, the newest full release is selected unless ?release= names
//     another; pre-releases are listed only when asked for (or named), and a release with nothing for
//     the board is listed but cannot be picked.
//
//   node test/web/flasher_lib_test.mjs [deps dir]
// (CI passes the folder with the web tests' libraries; this test needs none.) Exit status 0: every
// check passed.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const lib = await import(path.join(ROOT, 'flasher', 'lib.js'));

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
  return ok;
}

// ---- Tables -------------------------------------------------------------------------------------

const TYPES = { app: 0x00, data: 0x01 };
const SUBTYPES = {
  nvs: 0x02, ota: 0x00, factory: 0x00, ota_0: 0x10, ota_1: 0x11, spiffs: 0x82, littlefs: 0x83, coredump: 0x03,
};

// Entries of a partitions.csv-style text.
function csvEntries(text) {
  return text
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith('#'))
    .map((line) => {
      const [label, type, subtype, offset, size] = line.split(',').map((f) => f.trim());
      return {
        label, type: TYPES[type], subtype: SUBTYPES[subtype], offset: Number(offset), size: Number(size), flags: 0,
      };
    });
}

// The table as it sits at 0x8000: the entries, the MD5 entry, then 0xFF.
function tableBlob(entries) {
  const blob = new Uint8Array(lib.TABLE_SIZE).fill(0xff);
  const md5 = new lib.Md5();
  entries.forEach((e, i) => {
    const b = blob.subarray(i * 32, i * 32 + 32);
    b.fill(0);
    b.set([0xaa, 0x50, e.type, e.subtype]);
    const view = new DataView(b.buffer, b.byteOffset, 32);
    view.setUint32(4, e.offset, true);
    view.setUint32(8, e.size, true);
    b.set(new TextEncoder().encode(e.label), 12);
    md5.update(b);
  });
  const m = blob.subarray(entries.length * 32, entries.length * 32 + 32);
  m.set([0xeb, 0xeb]);
  const hex = md5.hex();
  for (let i = 0; i < 16; i++) m[16 + i] = parseInt(hex.slice(2 * i, 2 * i + 2), 16);
  return blob;
}

const oursCsv = fs.readFileSync(path.join(ROOT, 'partitions.csv'), 'utf8');
const ours = csvEntries(oursCsv);
// The table as the device holds it, parsed back: what classifyTable is given.
const read = (entries) => lib.parseTable(tableBlob(entries));
const withArea = (size) => ours.map((e) => (e.label === 'spiffs' ? { ...e, size } : e));
const withoutArea = ours.filter((e) => e.label !== 'spiffs');

// ---- AREA_BYTES ---------------------------------------------------------------------------------

const spiffs = ours.find((e) => e.label === 'spiffs');
check(spiffs && spiffs.size === lib.AREA_BYTES, "AREA_BYTES is partitions.csv's spiffs size");
const flashAreaH = fs.readFileSync(path.join(ROOT, 'lib', 'hal', 'FlashArea.h'), 'utf8');
const expected = /kExpectedBytes\s*=\s*(0x[0-9a-fA-F]+)/.exec(flashAreaH);
check(expected && Number(expected[1]) === lib.AREA_BYTES, 'AREA_BYTES is FlashArea::kExpectedBytes');

// ---- parseTable ---------------------------------------------------------------------------------

const oursRead = read(ours);
check(oursRead && oursRead.length === ours.length, 'our table reads back with every entry');
const damaged = tableBlob(ours);
damaged[8] ^= 1;  // the first entry's size
check(lib.parseTable(damaged) === null, 'a table whose MD5 entry does not match is no table');

// ---- classifyTable ------------------------------------------------------------------------------

const k = (entries) => lib.classifyTable(entries, ours);
check(k(oursRead).kind === 'ours', 'our table is ours');
check(k(null).kind === 'unknown', 'no table is unknown');

const x3 = k(read(withArea(0x100000)));  // the X3's original table: 1 MB at 0xc90000
check(x3.kind === 'foreign' && x3.areaBytes === 0x100000, 'a 1 MB spiffs partition is foreign, with its size');
const none = k(read(withoutArea));
check(none.kind === 'foreign' && none.areaBytes === 0, 'a table without spiffs is foreign, with 0 bytes of area');
const renamed = k(read(ours.map((e) => (e.label === 'spiffs' ? { ...e, label: 'storage' } : e))));
check(renamed.areaBytes === 0, 'a spiffs partition not labelled "spiffs" is not the area (EspPartitionFlash)');
const littlefs = k(read(ours.map((e) => (e.label === 'spiffs' ? { ...e, subtype: SUBTYPES.littlefs } : e))));
check(littlefs.areaBytes === 0, 'a littlefs partition labelled "spiffs" is not the area');
const otherApps = k(read(ours.map((e) => (e.label === 'app1' ? { ...e, size: 0x500000 } : e))));
check(otherApps.kind === 'foreign' && otherApps.areaBytes === lib.AREA_BYTES,
  'other app slots, our area size: foreign');
const onyx = k(read(csvEntries(fs.readFileSync(path.join(ROOT, 'partitions_picco.csv'), 'utf8'))));
check(onyx.kind === 'foreign' && onyx.onyx && onyx.areaBytes === 0x80000,
  "Onyx's Picco table (partitions_picco.csv): foreign, Onyx's, 512 KB of area");
check(![oursRead, read(withArea(0x100000)), read(withoutArea)].some((t) => k(t).onyx),
  "neither ours nor the X3/X4 tables read as Onyx's");

// ---- sizeText -----------------------------------------------------------------------------------

check(lib.sizeText(lib.AREA_BYTES) === '3.4 MB', 'AREA_BYTES reads 3.4 MB, as on the device');
check(lib.sizeText(0x100000) === '1.0 MB', '1 MB reads 1.0 MB');
check(lib.sizeText(80 * 1024) === '80 KB', '80 KB reads 80 KB');

// ---- layoutNote ---------------------------------------------------------------------------------

const C3 = 'ESP32-C3';
const S3 = 'ESP32-S3';
const has = (note, ...parts) => !!note && parts.every((p) => note.text.includes(p));

check(lib.layoutNote(k(oursRead), C3).level === 'ok', 'our table: an ok note');
check(lib.layoutNote(k(null), C3) === null, 'no table: no layout note (the page says why it could not tell)');

const x3OnC3 = lib.layoutNote(x3, C3);
check(has(x3OnC3, 'emergency mode', '1.0 MB', '3.4 MB', 'strongly recommend the full install'),
  'X3/X4 with 1 MB: emergency mode, both sizes, the full install recommended');
const noneOnC3 = lib.layoutNote(none, C3);
check(has(noneOnC3, 'emergency mode', 'no space', '3.4 MB'), 'X3/X4 without spiffs: emergency mode, no space');
const renamedOnC3 = lib.layoutNote(renamed, C3);
check(has(renamedOnC3, 'emergency mode', 'no space'), 'X3/X4 with an unlabelled spiffs partition: emergency mode');

for (const [name, note] of [['1 MB', lib.layoutNote(x3, S3)], ['no spiffs', lib.layoutNote(none, S3)]]) {
  check(has(note, 'PSRAM', 'Use the full install') && !note.text.includes('emergency'),
    `S3 board, ${name}: PSRAM, the full install, no emergency mode`);
}
const otherAppsOnC3 = lib.layoutNote(otherApps, C3);
check(has(otherAppsOnC3, 'not Witch Reader', 'Use the full install') && !otherAppsOnC3.text.includes('emergency'),
  'X3/X4, our area size but other app slots: the full install, no emergency mode');
const onyxNote = lib.layoutNote(onyx, S3);
check(has(onyxNote, "Onyx's") && onyxNote.choices && onyxNote.choices.length === 2,
  "Onyx's table: the note sets out two choices");
check(onyxNote.choices[0].startsWith('Full install') &&
  ['boot loader', 'recovery', 'over Wi-Fi', 'backup'].every((p) => onyxNote.choices[0].includes(p)),
  'full install: boot loader and recovery replaced, Wi-Fi updates, the backup as the way back');
check(onyxNote.choices[1].startsWith("Update only, Onyx's table kept") &&
  ['one app slot', 'cannot update itself', 'Update only again'].every((p) => onyxNote.choices[1].includes(p)),
  "Update only on Onyx's table: one slot, no self-update, later updates from this page");
check(!onyxNote.text.includes('emergency') && !onyxNote.choices.join(' ').includes('emergency'),
  "Onyx's table: no emergency mode");
check(![x3OnC3, noneOnC3, otherAppsOnC3].some((n) => n.text.includes('English')),
  'no note claims the interface stays in English');

// ---- Firmware image alone ----------------------------------------------------------------------

const target = (entries) => lib.appTarget(k(entries), entries);
const oursTarget = target(oursRead);
check(oursTarget && oursTarget.app === 0x10000 && oursTarget.otadata === 0xe000 && oursTarget.appBytes === 0x640000 &&
  !oursTarget.onyx, 'our table: app0 at 0x10000 (6.25 MB), otadata at 0xE000');
const onyxEntries = read(csvEntries(fs.readFileSync(path.join(ROOT, 'partitions_picco.csv'), 'utf8')));
const onyxTarget = target(onyxEntries);
check(onyxTarget && onyxTarget.app === lib.ONYX_APP_OFFSET && onyxTarget.appBytes === 0xe70000 &&
  onyxTarget.otadata === 0x15000 && onyxTarget.onyx, "Onyx's table: its app slot at 0xE0000, otadata at 0x15000");
for (const [name, entries] of [['the X3 table (1 MB)', read(withArea(0x100000))], ['no spiffs', read(withoutArea)],
  ['other app slots', read(ours.map((e) => (e.label === 'app1' ? { ...e, size: 0x500000 } : e)))]]) {
  check(target(entries) === null, `${name}: no firmware image alone, the full install only`);
}
check(lib.appTarget(k(null), null) === null, 'no table: no firmware image alone');

// boot_app0.bin (framework-arduinoespressif32/tools/partitions), written out: sequence 1, an erased
// label and state, CRC 0x4743989A; the second sector starts with sequence 0 and nothing else.
const bootApp0 = new Uint8Array(lib.OTADATA_SIZE).fill(0xff);
bootApp0.set([0x01, 0x00, 0x00, 0x00], 0);
bootApp0.set([0x9a, 0x98, 0x43, 0x47], 28);
bootApp0.set([0x00, 0x00, 0x00, 0x00], 0x1000);
const otadata = lib.otadataSelectingFirstSlot();
check(otadata.length === bootApp0.length && otadata.every((b, i) => b === bootApp0[i]),
  "otadataSelectingFirstSlot() is boot_app0.bin, byte for byte");

const app = new Uint8Array(0x1000).fill(0xe9);
const onyxPlan = lib.appWritePlan(onyxTarget, app, otadata);
check(!onyxPlan.error && onyxPlan.files.length === 2 && onyxPlan.files[0].address === 0xe0000 &&
  onyxPlan.files[0].data === app && onyxPlan.files[1].address === 0x15000 && onyxPlan.files[1].data === otadata,
  "Onyx's table: the image at 0xE0000 first, then otadata at 0x15000");
const oursPlan = lib.appWritePlan(oursTarget, app, otadata);
check(!oursPlan.error && oursPlan.files[0].address === 0x10000 && oursPlan.files[1].address === 0xe000,
  'our table: the image at 0x10000 first, then otadata at 0xE000');
check(!!lib.appWritePlan(oursTarget, new Uint8Array(0x640001), otadata).error,
  'an image larger than the slot is refused');

// ---- Backup -------------------------------------------------------------------------------------

check(lib.flashSizeBytes('16MB') === 16 * 1024 * 1024 && lib.flashSizeBytes('4MB') === 4 * 1024 * 1024 &&
  lib.flashSizeBytes('512KB') === 512 * 1024 && lib.flashSizeBytes(null) === null && lib.flashSizeBytes('16') === null,
  'flashSizeBytes reads esptool-js size names');
check(lib.backupFileName('picco', '9C:96:D5:E1:D8:2C', new Date(2026, 10, 5)) ===
  'picco-flash-9c96d5e1d82c-2026-11-05.bin', 'backupFileName: board, MAC, date');

const named = lib.parseBackupName('murphy-flash-9c96d5e1d82c-2026-11-15 (1).bin');
check(named && named.board === 'murphy' && named.mac === '9c:96:d5:e1:d8:2c' && named.date === '2026-11-15',
  "parseBackupName: board, MAC, day, also for a browser's second download");
check(lib.parseBackupName('firmware-picco.bin') === null && lib.parseBackupName('full-install-picco.bin') === null,
  'parseBackupName: release images are no backups');
const full = { kind: 'full' };
check(lib.isWholeFlashImage(full, 16 * 1024 * 1024) && lib.isWholeFlashImage(full, 4 * 1024 * 1024),
  'a full image of 16 MB or 4 MB is a copy of a whole flash');
check(!lib.isWholeFlashImage(full, 5775312 + 0x10000) && !lib.isWholeFlashImage({ kind: 'app' }, 16 * 1024 * 1024),
  'an install image (ends after the firmware) and an app image are not');

// A fake chip: `flash` bytes, a read that can be spoiled per call, and its MD5 of a range.
function fakeChip(size, spoil = () => null) {
  const flash = new Uint8Array(size);
  for (let i = 0; i < size; i++) flash[i] = (i * 31 + (i >> 9)) & 0xff;
  const calls = [];
  return {
    flash,
    calls,
    read: async (addr, len) => {
      calls.push(addr);
      const data = flash.slice(addr, addr + len);
      const how = spoil(addr, calls.filter((a) => a === addr).length);
      if (how === 'throw') throw new Error('No serial data received.');
      if (how === 'flip') data[7] ^= 0x40;
      if (how === 'short') return data.subarray(0, len - 1);
      return data;
    },
    chipMd5: async (addr, len) => lib.md5Hex(flash.subarray(addr, addr + len)).toUpperCase(),
  };
}
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const SIZE = 5 * 4096 + 100; // not a whole number of pieces
{
  const chip = fakeChip(SIZE);
  const done = [];
  const image = await lib.readWholeFlash({ ...chip, size: SIZE, chunk: 4096, onProgress: (d) => done.push(d) });
  check(same(image, chip.flash) && chip.calls.length === 6 && done.at(-1) === SIZE,
    'backup: six pieces, the last one short, the image equals the flash');
}
{
  const retries = [];
  const spoil = (addr, n) => (addr === 4096 && n === 1 ? 'flip' : addr === 8192 && n < 3 ? 'throw' : null);
  const chip = fakeChip(SIZE, spoil);
  const image = await lib.readWholeFlash({ ...chip, size: SIZE, chunk: 4096, onRetry: (a) => retries.push(a) });
  check(same(image, chip.flash) && retries.join() === '4096,8192,8192',
    'backup: a flipped bit and two failed reads are read again; the image equals the flash');
}
for (const how of ['flip', 'short', 'throw']) {
  const chip = fakeChip(SIZE, (addr) => (addr === 12288 ? how : null));
  let error = null;
  try {
    await lib.readWholeFlash({ ...chip, size: SIZE, chunk: 4096 });
  } catch (e) {
    error = e;
  }
  check(error && /0x3000/.test(error.message) && chip.calls.filter((a) => a === 12288).length === 3,
    `backup: a piece that stays bad (${how}) is tried three times, then the backup fails at its address`);
}

// ---- Releases -----------------------------------------------------------------------------------

// As releases.json lists them: a pre-release newer than the latest full release first, then full
// releases newest first; an older one without the Picco, and one without any full image.
const img = { file: 'x', size: 1, sha256: '0' };
const boards = (ids, full = true) =>
  Object.fromEntries(lib.BOARDS.map((b) => [b.id, ids.includes(b.id) ? { app: img, full: full ? img : null }
    : { app: null, full: null }]));
const ALL = lib.BOARDS.map((b) => b.id);
const pre = { tag: '2.54RC', name: '2.54 RC', prerelease: true, boards: boards(ALL) };
const latest = { tag: '2.53', name: '2.53 - Latest', prerelease: false, boards: boards(ALL) };
const noPicco = { tag: '2.51', name: '2.51', prerelease: false, boards: boards(ALL.filter((id) => id !== 'picco')) };
const appOnly = { tag: '2.50', name: '', prerelease: false, boards: boards(['x3x4', 'x4pro'], false) };
const releases = [pre, latest, noPicco, appOnly];

check(lib.defaultRelease(releases) === latest,
  'releases: the newest full release is the default, not a newer pre-release');
check(lib.defaultRelease([pre]) === pre, 'releases: a site with only a pre-release selects it');
check(lib.defaultRelease([]) === null, 'releases: none on the site, none selected');

check(lib.releaseHasBoard(latest, 'picco') && !lib.releaseHasBoard(noPicco, 'picco'),
  'releases: a release has a board when it carries an image for it');
check(lib.releaseHasBoard(appOnly, 'x3x4') && !lib.releaseHasBoard(appOnly, 'lilygo'),
  'releases: a firmware image alone is enough; a board with neither image has nothing');

const opened = (wanted) => lib.initialRelease(releases, wanted);
check(opened(null).release === latest && !opened(null).showPre && !opened(null).missing,
  'releases: no ?release= opens the default with pre-releases hidden');
check(opened('2.51').release === noPicco && !opened('2.51').showPre, 'releases: ?release= opens that full release');
check(opened('2.54RC').release === pre && opened('2.54RC').showPre,
  'releases: ?release= of a pre-release lists the pre-releases');
check(opened('1.0').release === latest && opened('1.0').missing === '1.0',
  'releases: a tag the site lacks opens the default and says so');

check(lib.releaseKind(releases, latest) === 'latest' && lib.releaseKind(releases, noPicco) === 'older' &&
  lib.releaseKind(releases, pre) === 'pre', 'releases: latest, older and pre-release are told apart');

const tags = (opts) => lib.releaseOptions(releases, opts).map((o) => o.tag).join(' ');
check(tags({ boardId: 'x3x4', showPre: false }) === '2.53 2.51 2.50',
  'releases: pre-releases are not listed unless asked for');
check(tags({ boardId: 'x3x4', showPre: true }) === '2.54RC 2.53 2.51 2.50',
  'releases: asked for, pre-releases come first');
const picco = lib.releaseOptions(releases, { boardId: 'picco', showPre: true });
check(picco.filter((o) => o.disabled).map((o) => o.tag).join(' ') === '2.51 2.50',
  'releases: one with nothing for the board is listed, but disabled');
const text = (tag) => picco.find((o) => o.tag === tag).text;
check(text('2.53') === '2.53 - Latest (latest)' && text('2.54RC') === '2.54 RC (pre-release)' &&
  text('2.51') === '2.51 (nothing for this device)' && text('2.50') === '2.50 (nothing for this device)',
  'releases: entries carry their name (the tag when it has none) and what sets them apart');

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
