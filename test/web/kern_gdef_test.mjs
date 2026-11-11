// The browser converter's kerning honours GPOS lookup flags against GDEF glyph classes, as the Python
// converters (lib/EpdFont/scripts/kern_extract.py) and the firmware's GPOS reader do: a lookup whose
// flags skip a glyph kerns no pair holding it. Fonts whose lookups skip nothing a pair holds keep
// their whole pair list (the hashes are those of the converter before GDEF-aware kerning).
//
//   node test/web/kern_gdef_test.mjs [deps dir, unused]
//
// test_kern_extract.py checks the browser converter against the Python one on fonts built for each
// flag; this test needs node alone.
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const { SfntFont } = await import(join(repo, 'tools/ftwasm/js/otf-tables.mjs'));
const { prepareKerning } = await import(join(repo, 'tools/ftwasm/js/cpfont-writer.mjs'));

let failures = 0;
const check = (ok, what) => {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
};

const read = (relative) => new Uint8Array(readFileSync(join(repo, relative)));
const SERIF = 'lib/EpdFont/builtinFonts/source/SourceSerif4Caption/SourceSerif4Caption-Regular.ttf';

/** Every pair the font kerns among the glyphs its cmap maps: Map<left*65536+right, units>. */
function kerning(bytes) {
  const font = new SfntFont(bytes, 'font');
  return { font, raw: prepareKerning(font, [...font.bestCmap().keys()]).raw };
}

/** sha256 (16 hex digits) of 'left right units' lines in glyph order, as test_kern_extract.py hashes them. */
function pairListHash(raw) {
  const lines = [...raw].sort((a, b) => a[0] - b[0]).map(([k, v]) => `${Math.floor(k / 65536)} ${k % 65536} ${v}`);
  return createHash('sha256').update(lines.join('\n')).digest('hex').slice(0, 16);
}

/** A copy whose pair adjustment lookups (types 2 and 9) have `flags`; its GDEF tag renamed away if asked. */
function withPairLookupFlags(bytes, flags, dropGdef = false) {
  const out = bytes.slice();
  const dv = new DataView(out.buffer);
  let gpos = 0;
  for (let i = 0; i < dv.getUint16(4); i++) {
    const rec = 12 + 16 * i;
    const tag = String.fromCharCode(...out.subarray(rec, rec + 4));
    if (tag === 'GPOS') gpos = dv.getUint32(rec + 8);
    if (tag === 'GDEF' && dropGdef) out.set([0x78, 0x78, 0x78, 0x78], rec); // 'xxxx': no GDEF
  }
  const list = gpos + dv.getUint16(gpos + 8);
  for (let i = 0; i < dv.getUint16(list); i++) {
    const lookup = list + dv.getUint16(list + 2 + 2 * i);
    const type = dv.getUint16(lookup);
    if (type === 2 || type === 9) dv.setUint16(lookup + 2, flags);
  }
  return out;
}

const pair = ({ font, raw }, left, right) => {
  const cmap = font.bestCmap();
  return raw.get(cmap.get(left.codePointAt(0)) * 65536 + cmap.get(right.codePointAt(0))) ?? 0;
};

// Caecilia LT's kern lookups set 0x000E (skip base glyphs, ligatures and marks) over Latin letters its GDEF
// classes as base glyphs: HarfBuzz kerns none of them. Source Serif given the same flags kerns none either.
const serif = read(SERIF);
const plain = kerning(serif);
check(pair(plain, 'A', 'V') === -120 && pair(plain, 'T', 'o') === -40, 'Source Serif kerns A/V -120 and T/o -40');
const caecilia = kerning(withPairLookupFlags(serif, 0x000E));
check(pair(caecilia, 'A', 'V') === 0 && pair(caecilia, 'T', 'o') === 0, "given Caecilia's flags, A/V and T/o kern 0");
check(pairListHash(kerning(withPairLookupFlags(serif, 0x000E, true)).raw) === pairListHash(plain.raw),
  'the same flags without GDEF change no pair');

// Noto Sans Grantha 2.001: sha before the anusvara, a mark, -40 units in a lookup that ignores marks (0x0508).
const grantha = kerning(read('test/fixtures/fonts/NotoSansGrantha-Kern-Regular.ttf'));
const granthaCmap = grantha.font.bestCmap();
const sha = granthaCmap.get(0x11336), anusvara = granthaCmap.get(0x11302);
check(sha && anusvara && !grantha.raw.has(sha * 65536 + anusvara), 'Grantha kerns no mark its lookup ignores');
check(kerning(withPairLookupFlags(read('test/fixtures/fonts/NotoSansGrantha-Kern-Regular.ttf'), 0x0508, true))
  .raw.get(sha * 65536 + anusvara) === -40, 'without its GDEF the pair kerns -40');

// Whole pair lists as before: Ubuntu has no GDEF, Source Serif's and Noto Sans's lookups skip only marks.
const BEFORE = [
  ['lib/EpdFont/builtinFonts/source/Ubuntu/Ubuntu-Regular.ttf', 56975, '5dfdacbdfcbe250a'],
  [SERIF, 115665, '795b2f38f93a3a1b'],
  ['lib/EpdFont/builtinFonts/source/NotoSans/NotoSans-Regular.ttf', 36062, '39ca7c4a0d09b2a9'],
];
for (const [path, count, digest] of BEFORE) {
  const { raw } = kerning(read(path));
  check(raw.size === count && pairListHash(raw) === digest, `${path.split('/').pop()}: the pair list as before`);
}

if (failures) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
