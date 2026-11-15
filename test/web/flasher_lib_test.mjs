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
//     the full install (Onyx's boot loader and recovery replaced, self-updates) or keeping the table
//     (no self-update, updates over USB).
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
check(onyxNote.choices[1].startsWith("Keeping Onyx's table") &&
  ['one app slot', 'cannot update itself', 'over USB'].every((p) => onyxNote.choices[1].includes(p)),
  "keeping Onyx's table: one slot, no self-update, updates over USB");
check(!onyxNote.text.includes('emergency') && !onyxNote.choices.join(' ').includes('emergency'),
  "Onyx's table: no emergency mode");
check(![x3OnC3, noneOnC3, otherAppsOnC3].some((n) => n.text.includes('English')),
  'no note claims the interface stays in English');

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('all checks passed');
