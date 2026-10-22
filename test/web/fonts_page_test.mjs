#!/usr/bin/env node
// Tests the Font Manager page (FontsPage.html) against the font API's answers: an installed outline
// family shows "all sizes", and an install the reader does not list, or the device refuses, says so.
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

const HTML_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'src', 'network', 'html');

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
function loadPage(answer) {
  const html = resolveIncludes(fs.readFileSync(path.join(HTML_DIR, 'FontsPage.html'), 'utf8'));
  const beforeParse = (w) => {
    w.fetch = async (u, init = {}) => answer(String(u), init) || response(404, '');
    w.alert = () => {};
    w.confirm = () => true;
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

console.log(failures ? `${failures} check(s) FAILED` : 'all passed');
process.exit(failures ? 1 : 0);
