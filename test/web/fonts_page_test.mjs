#!/usr/bin/env node
// Tests the Font Manager page (FontsPage.html) against the font API's answers: an installed outline
// family shows "all sizes", and an install the reader does not list, or the device refuses, says so;
// the language packs are a list of their own; a TrueType family is uploaded as it is.
//
//   node test/web/fonts_page_test.mjs <dir with node_modules/jsdom>
// (the folder CI installs jsdom into, see .github/workflows/ci.yml). Exit status 0: every check passed.

import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const deps = process.argv[2];
if (!deps) {
  console.error('usage: node test/web/fonts_page_test.mjs <dir with node_modules/jsdom>');
  process.exit(2);
}
const { JSDOM, VirtualConsole } = createRequire(path.join(path.resolve(deps), 'noop.js'))('jsdom');

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const HTML_DIR = path.join(REPO, 'src', 'network', 'html');
// The tracked Noto Sans sources, as an upload's TrueType files.
const notoSans = (style) => fs.readFileSync(path.join(REPO, 'lib/EpdFont/builtinFonts/source/NotoSans', `NotoSans-${style}.ttf`));

let failures = 0;
function check(ok, what) {
  console.log((ok ? 'ok   ' : 'FAIL ') + what);
  if (!ok) failures++;
  return ok;
}
function same(actual, expected, what) {
  check(actual === expected, what + (actual === expected ? '' : ` (got ${JSON.stringify(actual)}, want ${JSON.stringify(expected)})`));
}

// Unhandled rejections from a page's start-up (requests the fake refuses) are not under test.
process.on('unhandledRejection', () => {});

function resolveIncludes(html) {
  for (let depth = 0; depth < 8; depth++) {
    let count = 0;
    html = html.replace(/<!--\s*#include\s+file="([^"]+)"\s*-->/g, (_, rel) => {
      count++;
      return fs.readFileSync(path.join(HTML_DIR, rel), 'utf8');
    });
    if (!count) break;
  }
  return html;
}

function response(status, body) {
  return {
    ok: status >= 200 && status < 300,
    status,
    statusText: String(status),
    json: async () => (typeof body === 'string' ? JSON.parse(body) : body),
    text: async () => (typeof body === 'string' ? body : JSON.stringify(body)),
  };
}

// Loads the Font Manager with a fake fetch only; `answer(url, init)` answers a request (undefined: 404).
function loadPage(answer, confirmAnswer = true, asked = []) {
  const html = resolveIncludes(fs.readFileSync(path.join(HTML_DIR, 'FontsPage.html'), 'utf8'));
  const beforeParse = (w) => {
    w.fetch = async (u, init = {}) => answer(String(u), init) || response(404, '');
    w.alert = () => {};
    w.confirm = (message) => { asked.push(message); return confirmAnswer; };
    // What the page loads on demand, and what jsdom's Blob lacks.
    w.eval(fs.readFileSync(path.join(HTML_DIR, 'js', 'epub-fonts.js'), 'utf8'));
    w.Blob.prototype.arrayBuffer = function () {
      return new Promise((resolve) => {
        const reader = new w.FileReader();
        reader.onload = () => resolve(reader.result);
        reader.readAsArrayBuffer(this);
      });
    };
  };
  const dom = new JSDOM(html, { url: 'http://reader.test/fonts', runScripts: 'dangerously', virtualConsole: new VirtualConsole(), beforeParse });
  return { dom, w: dom.window };
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 5));
async function until(condition, ms = 3000) {
  for (const end = Date.now() + ms; Date.now() < end; await tick()) if (condition()) return true;
  return condition();
}

const REMOTE = {
  ok: true, baseUrl: 'https://fonts.test/',
  families: [{ name: 'NotoSerifKR', description: 'Korean serif', installed: false, hasUpdate: false, totalSize: 8000000, fileCount: 1 }],
};

// Clicks the Download button of NotoSerifKR with /api/fonts/download answering `download`; resolves to #remoteStatus.
async function installKorean(download) {
  const { w, dom } = loadPage((u) => {
    if (u === '/api/fonts') return response(200, { families: [] });
    if (u === '/api/fonts/manifest') return response(200, REMOTE);
    if (u === '/api/fonts/download') return response(download.ok === false ? 500 : 200, download);
  });
  await until(() => w.document.querySelector('#remoteFamilies button[data-family]'));
  w.document.querySelector('#remoteFamilies button[data-family]').click();
  const status = w.document.getElementById('remoteStatus');
  await until(() => /^(Installed|Install failed)/.test(status.textContent));
  const result = { text: status.textContent, cls: status.className };
  dom.window.close();
  return result;
}

console.log('- the installed list');
{
  const { w, dom } = loadPage((u) => u === '/api/fonts' ? response(200, { families: [
    { name: 'NotoSansJP', outline: true, sizes: [], files: [{ name: 'NotoSansJP-Regular.otf', size: 1690040 }] },
    { name: 'Literata', outline: false, sizes: [10, 12], files: [{ name: 'Literata_10.cpfont', size: 272833 }, { name: 'Literata_12.cpfont', size: 341352 }] },
  ] }) : undefined);
  await until(() => w.document.querySelectorAll('.family-meta').length === 2);
  const metas = [...w.document.querySelectorAll('#families .family-meta')].map((e) => e.textContent);
  same(metas[0], 'all sizes - 1.6 MB', 'an outline family shows "all sizes"');
  same(metas[1], '10, 12pt - 266 KB + 333 KB', 'a bitmap family shows its sizes');
  dom.window.close();
}

console.log('- an install the reader does not list');
{
  const r = await installKorean({ ok: true, installedCount: 1, notListed: ['NotoSerifKR'] });
  same(r.text, 'Installed "NotoSerifKR", but it is too large for this reader\'s font area: the reader does not list it.', 'the status says so');
  check(/status-err/.test(r.cls), 'and is an error status');
}

console.log('- a refused download');
{
  const r = await installKorean({ ok: false, error: 'Not enough space on the SD card: 8.0 MB needed, 2.1 MB free' });
  same(r.text, 'Install failed for "NotoSerifKR": Not enough space on the SD card: 8.0 MB needed, 2.1 MB free', 'the status shows the device\'s error');
}

console.log('- a folder of the reader\'s own under a hosted name');
for (const agree of [false, true]) {
  const asked = [];
  let downloads = 0;
  const { w, dom } = loadPage((u) => {
    if (u === '/api/fonts') return response(200, { families: [] });
    if (u === '/api/fonts/manifest') return response(200, { ...REMOTE, families: [{ ...REMOTE.families[0], installed: true, hasUpdate: false, ownFiles: true }] });
    if (u === '/api/fonts/download') { downloads++; return response(200, { ok: true, installedCount: 1, notListed: [] }); }
  }, agree, asked);
  await until(() => w.document.querySelector('#remoteFamilies button[data-family]'));
  const button = w.document.querySelector('#remoteFamilies button[data-family]');
  same(button.textContent, 'Reinstall', 'the family offers Reinstall');
  button.click();
  await until(() => asked.length > 0);
  await until(() => downloads > 0, 200);
  check(asked.length === 1 && asked[0].includes('NotoSerifKR') && /replace/i.test(asked[0]), 'it asks, naming the family and saying its files are replaced');
  same(downloads, agree ? 1 : 0, agree ? 'OK sends the download' : 'Cancel sends no download');
  dom.window.close();
}

const PACKED = { ...REMOTE, families: [
  { name: 'ChineseUI', description: 'Simplified Chinese in the interface', interfacePack: true, installed: true, hasUpdate: true, totalSize: 3000000, fileCount: 1 },
  REMOTE.families[0],
  { name: 'Literata', description: 'Serif', installed: true, hasUpdate: true, totalSize: 900000, fileCount: 5 },
  { name: 'Language-German', description: 'Menus and messages in German', language: true, installed: true, hasUpdate: false, inUse: true, totalSize: 14000, fileCount: 1 },
  { name: 'Hyphenation-Dutch', description: 'Hyphenation for books in Dutch', hyphenation: true, installed: false, hasUpdate: false, totalSize: 64313, fileCount: 1 },
  { name: 'Hyphenation-German', description: 'Hyphenation for books in German', hyphenation: true, installed: true, hasUpdate: true, totalSize: 206283, fileCount: 1 },
] };

// The rows and headings of a list, in order: a row by its family, a heading as "# <title>".
function listOrder(list) {
  return [...list.children].filter((e) => !e.classList.contains('family-note')).map((e) =>
    e.classList.contains('family-group') ? '# ' + e.textContent : (e.querySelector('h3') || {}).textContent || e.className);
}

console.log('- the language packs, a list of their own');
{
  const { w, dom } = loadPage((u) => {
    if (u === '/api/fonts') return response(200, { families: [] });
    if (u === '/api/fonts/manifest') return response(200, PACKED);
  });
  await until(() => w.document.querySelectorAll('#packFamilies button[data-family]').length === 4);
  const fonts = w.document.getElementById('remoteFamilies');
  same(listOrder(fonts).join(','), 'NotoSerifKR,Literata', 'Download fonts lists the fonts alone');
  same(fonts.querySelectorAll('.family-group').length, 0, 'under no heading');
  const packs = w.document.getElementById('packFamilies');
  same(listOrder(packs).join(','), '# Interface languages,Language-German,# Book titles in Chinese, Japanese and Korean,ChineseUI,' +
    '# Hyphenation,Hyphenation-Dutch,Hyphenation-German', 'Language packs lists each kind of pack under its heading');
  same(packs.querySelectorAll('.family-note').length, 3, 'each heading with its note');
  same([...packs.querySelectorAll('button[data-family]')].map((b) => b.textContent).join(','), 'Reinstall,Update,Download,Update',
    'a pack is downloaded like a font');
  same(w.document.getElementById('updateCount').textContent, '1', 'the fonts count their own updates');
  same(w.document.getElementById('packUpdateCount').textContent, '2', 'the packs theirs');
  same(w.document.querySelector('.cat-link[data-panel="packs"]').closest('.cat-list').previousElementSibling.textContent,
    'Languages', 'the packs have their own group in the menu');
  dom.window.close();
}

console.log('- each list\'s "all updates" and status');
for (const [button, list, statusId, other] of [['downloadAllRemote', 'fonts', 'remoteStatus', 'packStatus'], ['updateAllPacks', 'packs', 'packStatus', 'remoteStatus']]) {
  const bodies = [];
  const { w, dom } = loadPage((u, init) => {
    if (u === '/api/fonts') return response(200, { families: [] });
    if (u === '/api/fonts/manifest') return response(200, PACKED);
    if (u === '/api/fonts/download') { bodies.push(JSON.parse(init.body)); return response(200, { ok: true, installedCount: 2, notListed: [] }); }
  });
  await until(() => w.document.querySelectorAll('#packFamilies button[data-family]').length === 4);
  w.document.getElementById(button).click();
  const status = w.document.getElementById(statusId);
  await until(() => /^(Installed|Updated)/.test(status.textContent));
  same(JSON.stringify(bodies[0]), JSON.stringify({ all: true, list }), `${button} asks for the ${list} only`);
  same(status.textContent, list === 'packs' ? 'Updated 2 pack(s).' : 'Installed/updated 2 family(s).', 'and says so in its own status');
  same(w.document.getElementById(other).textContent, 'Waiting for action...', 'the other list\'s status is untouched');
  dom.window.close();
}

console.log('- a pack\'s download reports beside the packs');
{
  const { w, dom } = loadPage((u) => {
    if (u === '/api/fonts') return response(200, { families: [] });
    if (u === '/api/fonts/manifest') return response(200, PACKED);
    if (u === '/api/fonts/download') return response(200, { ok: true, installedCount: 1, notListed: [] });
  });
  await until(() => w.document.querySelectorAll('#packFamilies button[data-family]').length === 4);
  w.document.querySelector('#packFamilies button[data-family="Hyphenation-Dutch"]').click();
  const status = w.document.getElementById('packStatus');
  await until(() => /^Installed/.test(status.textContent));
  same(status.textContent, 'Installed "Hyphenation-Dutch".', 'the packs\' status says it');
  dom.window.close();
}

console.log('- deleting a pack');
for (const [name, agree, inUse, body, asks, done] of [
  ['Hyphenation-German', true, false, { family: 'Hyphenation-German', pack: true }, /^Delete "Hyphenation-German"\? Books in its language are no longer hyphenated\.$/,
    'Deleted "Hyphenation-German".'],
  ['Hyphenation-German', true, true, { family: 'Hyphenation-German', pack: true }, /no longer hyphenated\. It is a pack of the interface language: the reader may offer it again/,
    'Deleted "Hyphenation-German".'],
  ['Language-German', true, true, { family: 'Language-German', pack: true }, /interface is in this language: without its pack the menus and messages are English from the reader's next start\.$/,
    'Deleted "Language-German". The interface is English from the reader\'s next start.'],
  ['ChineseUI', true, false, { family: 'ChineseUI' }, /^Delete "ChineseUI"\? Book titles in its script show boxes outside a book/, 'Deleted "ChineseUI".'],
  ['Hyphenation-German', false, false, null, /^Delete "Hyphenation-German"\?/, null],
]) {
  const asked = [];
  const deletes = [];
  const families = PACKED.families.map((f) => (f.name === name ? { ...f, inUse } : f));
  const { w, dom } = loadPage((u, init) => {
    if (u === '/api/fonts') return response(200, { families: [] });
    if (u === '/api/fonts/manifest') return response(200, { ...PACKED, families });
    if (u === '/api/fonts/delete') { deletes.push(JSON.parse(init.body)); return response(200, { ok: true }); }
  }, agree, asked);
  await until(() => w.document.querySelectorAll('#packFamilies button[data-family]').length === 4);
  same([...w.document.querySelectorAll('button[data-delete-pack]')].map((b) => b.dataset.deletePack).join(','),
    'Language-German,ChineseUI,Hyphenation-German', 'an installed pack has Delete, a missing one none, no font has it');
  w.document.querySelector(`button[data-delete-pack="${name}"]`).click();
  await until(() => asked.length > 0);
  check(asks.test(asked[0] || ''), `${name}${inUse ? ' (in use)' : ''}: the question says what it changes (${asked[0]})`);
  const status = w.document.getElementById('packStatus');
  if (agree) {
    await until(() => /^Deleted/.test(status.textContent));
    same(JSON.stringify(deletes[0]), JSON.stringify(body), 'the delete goes as the reader deletes that kind');
    same(status.textContent, done, 'the packs\' status says it');
  } else {
    await until(() => deletes.length > 0, 200);
    same(deletes.length, 0, 'Cancel deletes nothing');
  }
  dom.window.close();
}

{
  const { w, dom } = loadPage((u) => {
    if (u === '/api/fonts') return response(200, { families: [] });
    if (u === '/api/fonts/manifest') return response(200, PACKED);
    if (u === '/api/fonts/delete') return response(500, { ok: false, error: 'Delete failed' });
  });
  await until(() => w.document.querySelectorAll('button[data-delete-pack]').length === 3);
  w.document.querySelector('button[data-delete-pack="Hyphenation-German"]').click();
  const status = w.document.getElementById('packStatus');
  await until(() => /^Could not/.test(status.textContent));
  same(status.textContent, 'Could not delete "Hyphenation-German": Delete failed', 'a refused delete shows the device\'s reason');
  check(/status-err/.test(status.className), 'as an error');
  dom.window.close();
}

// Picks `files` ([name, bytes]) in Upload a font; resolves once the page has read them.
async function pickUpload(w, files) {
  const input = w.document.getElementById('fontFiles');
  Object.defineProperty(input, 'files', { value: files.map(([name, bytes]) => new w.File([bytes], name)), configurable: true });
  input.dispatchEvent(new w.Event('change'));
  await until(() => !w.document.getElementById('uploadBtn').disabled || /status-err/.test(w.document.getElementById('status').className));
}

// The Font Manager with the given installed families, an upload answering `answer(n)` for its n-th file.
function uploadPage(installed, answer, confirmAnswer = true, asked = []) {
  const uploads = [];
  const page = loadPage((u, init) => {
    if (u === '/api/fonts') return response(200, { families: installed });
    if (u === '/api/fonts/manifest') return response(200, REMOTE);
    if (u === '/api/fonts/upload') {
      uploads.push({ family: init.body.get('family'), name: init.body.get('file').name, size: init.body.get('file').size });
      const a = answer(uploads.length);
      return response(a.ok === false ? 400 : 200, a);
    }
  }, confirmAnswer, asked);
  return { ...page, uploads };
}

console.log('- a TrueType family uploaded as it is');
{
  const { w, dom, uploads } = uploadPage([], () => ({ ok: true, listed: true }));
  await until(() => w.document.getElementById('installedCount').textContent === '0');
  await pickUpload(w, [['NotoSans-Bold.ttf', notoSans('Bold')], ['NotoSans-Regular.ttf', notoSans('Regular')]]);
  const rows = [...w.document.querySelectorAll('#uploadStyleTable .style-row')].map((r) => r.textContent);
  same(rows.join('|'), 'RegularNotoSans-Regular.ttf|BoldNotoSans-Bold.ttf|Italicnone|Bold italicnone', 'each file\'s style is read from the font');
  same(w.document.getElementById('uploadName').value, 'NotoSans', 'the family name comes from the font');
  w.document.getElementById('uploadBtn').click();
  const status = w.document.getElementById('status');
  await until(() => /^(Uploaded|Could not)/.test(status.textContent));
  same(uploads.map((u) => u.family + '/' + u.name).join(','), 'NotoSans/NotoSans-Regular.ttf,NotoSans/NotoSans-Bold.ttf',
    'each style goes up as <family>-<Style>.ttf, regular first');
  same(uploads[0].size, notoSans('Regular').length, 'the file as it is');
  check(/^Uploaded "NotoSans": 2 files, .*drawn at every size\.$/.test(status.textContent) && /status-ok/.test(status.className),
    'the status says it is uploaded and drawn at every size (' + status.textContent + ')');
  dom.window.close();
}

console.log('- an OpenType CFF file keeps .otf, and the family name can be changed');
{
  const { w, dom, uploads } = uploadPage([], () => ({ ok: true, listed: true }));
  const cff = Buffer.from(notoSans('Italic'));
  cff.write('OTTO', 0, 'latin1');  // the page goes by the sfnt version alone
  await pickUpload(w, [['Some Font.otf', cff]]);
  w.document.getElementById('uploadName').value = 'My_Font';
  w.document.getElementById('uploadBtn').click();
  await until(() => uploads.length === 1);
  same(uploads[0].name, 'My_Font-Italic.otf', 'an italic CFF file goes up as <family>-Italic.otf');
  dom.window.close();
}

console.log('- a variable font: uploaded, not converted');
{
  // A table renamed fvar: what tells the page a font is variable.
  const variable = Buffer.from(notoSans('Regular'));
  for (let i = 0; i < variable.readUInt16BE(4); i++) {
    if (variable.toString('latin1', 12 + i * 16, 16 + i * 16) === 'post') variable.write('fvar', 12 + i * 16, 'latin1');
  }
  const { w, dom } = uploadPage([], () => ({ ok: true, listed: true }));
  await pickUpload(w, [['NotoSans[wght].ttf', variable]]);
  same(w.document.querySelector('#uploadStyleTable .style-file').textContent,
    'NotoSans[wght].ttf (a variable font: drawn in its default style)', 'the upload takes it, saying how it is drawn');
  check(!w.document.getElementById('uploadBtn').disabled, 'and offers Upload');
  const input = w.document.getElementById('convertFiles');
  Object.defineProperty(input, 'files', { value: [new w.File([variable], 'NotoSans[wght].ttf')], configurable: true });
  input.dispatchEvent(new w.Event('change'));
  const status = w.document.getElementById('convertStatus');
  await until(() => /status-err/.test(status.className));
  same(status.textContent, 'NotoSans[wght].ttf is a variable font; choose the static files of the family.', 'the converter refuses it, as before');
  dom.window.close();
}

console.log('- too large for the font area');
{
  const { w, dom } = uploadPage([], (n) => ({ ok: true, listed: n < 2 }));
  await pickUpload(w, [['NotoSans-Regular.ttf', notoSans('Regular')], ['NotoSans-Italic.ttf', notoSans('Italic')]]);
  w.document.getElementById('uploadBtn').click();
  const status = w.document.getElementById('status');
  await until(() => /^(Uploaded|Could not)/.test(status.textContent));
  check(/too large for this reader's font area: the reader does not list it\.$/.test(status.textContent) && /status-err/.test(status.className),
    'the last answer\'s listed=false is an error status (' + status.textContent + ')');
  dom.window.close();
}

console.log('- the device refuses a file');
{
  const { w, dom } = uploadPage([], () => ({ ok: false, error: 'Not a font the reader can draw: a variable CFF2 font, or one without a Unicode character map' }));
  await pickUpload(w, [['NotoSans-Regular.ttf', notoSans('Regular')]]);
  w.document.getElementById('uploadBtn').click();
  const status = w.document.getElementById('status');
  await until(() => /^(Uploaded|Could not)/.test(status.textContent));
  same(status.textContent, 'Could not upload: NotoSans-Regular.ttf: Not a font the reader can draw: a variable CFF2 font, or one without a Unicode character map',
    'the device\'s reason is shown');
  dom.window.close();
}

console.log('- a family already on the reader');
{
  // A .cpfont family of that name: refused before any upload, whatever the case of the name.
  const cpfont = uploadPage([{ name: 'notosans', outline: false, sizes: [12], files: [] }], () => ({ ok: true, listed: true }));
  await until(() => cpfont.w.document.getElementById('installedCount').textContent === '1');
  await pickUpload(cpfont.w, [['NotoSans-Regular.ttf', notoSans('Regular')]]);
  cpfont.w.document.getElementById('uploadBtn').click();
  const status = cpfont.w.document.getElementById('status');
  await until(() => /status-err/.test(status.className));
  check(/^The reader has a \.cpfont family "notosans": a TrueType or OpenType file would not be used beside it\./.test(status.textContent),
    'a TrueType file is not put beside a .cpfont family (' + status.textContent + ')');
  same(cpfont.uploads.length, 0, 'and nothing is sent');
  cpfont.dom.window.close();

  // A TrueType family of that name: added to once the reader agrees.
  for (const agree of [false, true]) {
    const asked = [];
    const ttf = uploadPage([{ name: 'NotoSans', outline: true, sizes: [], files: [] }], () => ({ ok: true, listed: true }), agree, asked);
    await until(() => ttf.w.document.getElementById('installedCount').textContent === '1');
    await pickUpload(ttf.w, [['NotoSans-Italic.ttf', notoSans('Italic')]]);
    ttf.w.document.getElementById('uploadBtn').click();
    await until(() => asked.length > 0);
    await until(() => ttf.uploads.length > 0, 200);
    check(asked.length === 1 && asked[0].includes('"NotoSans"') && /added/.test(asked[0]), 'it asks first, naming the family');
    same(ttf.uploads.length, agree ? 1 : 0, agree ? 'OK uploads into it' : 'Cancel uploads nothing');
    ttf.dom.window.close();
  }
}

console.log('- .cpfont files and font files are not mixed');
{
  const { w, dom } = uploadPage([], () => ({ ok: true, listed: true }));
  await pickUpload(w, [['NotoSans-Regular.ttf', notoSans('Regular')], ['NotoSans_12.cpfont', Buffer.from('CPFONT\0\0')]]);
  const status = w.document.getElementById('status');
  same(status.textContent, 'Choose TrueType, OpenType or WOFF files, or .cpfont files, not both.', 'one kind at a time');
  check(w.document.getElementById('uploadBtn').disabled, 'and Upload stays off');
  dom.window.close();
}

console.log('- Convert refuses a name a TrueType family has');
{
  const { w, dom } = uploadPage([{ name: 'NotoSans', outline: true, sizes: [], files: [] }], () => ({ ok: true }));
  await until(() => w.document.getElementById('installedCount').textContent === '1');
  const input = w.document.getElementById('convertFiles');
  Object.defineProperty(input, 'files', { value: [new w.File([notoSans('Regular')], 'NotoSans-Regular.ttf')], configurable: true });
  input.dispatchEvent(new w.Event('change'));
  await until(() => !w.document.getElementById('convertBtn').disabled);
  w.document.getElementById('convertBtn').click();
  const status = w.document.getElementById('convertStatus');
  await until(() => /status-err/.test(status.className));
  check(/^The reader has a TrueType family "NotoSans": \.cpfont files would hide its files\./.test(status.textContent),
    'before converting anything (' + status.textContent + ')');
  dom.window.close();
}

console.log(failures ? `${failures} check(s) FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
